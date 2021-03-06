#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern "C" {
#include "BIOS.h"
#include "stm32f10x.h"
#include "ds203_io.h"
#include "mathutils.h"
#include "Interrupt.h"
#include "irq.h"
#include "buttons.h"
#include "lcd.h"
}

#include "dsosignalstream.hh"
#include "xposhandler.hh"
#include "drawable.hh"
#include "textdrawable.hh"
#include "signalgraph.hh"
#include "breaklines.hh"
#include "window.hh"
#include "cursor.hh"
#include "timemeasure.hh"
#include "grid.hh"
#include "menudrawable.hh"
 
//define some colors
#define WHITE   0xFFFF
#define BLACK   0x0000
#define GREY    0x8410

// For some reason, the headers don't have these registers
#define FSMC_BCR1   (*((vu32 *)(0xA0000000+0x00)))
#define FSMC_BTR1   (*((vu32 *)(0xA0000000+0x04)))
#define FSMC_BCR2   (*((vu32 *)(0xA0000008+0x00)))
#define FSMC_BTR2   (*((vu32 *)(0xA0000008+0x04)))

// GPIOC->BSRR values to toggle GPIOC5
static const uint32_t hl_set[2] = {1 << (16 + 5), 1 << 5};

// FIFO for stuff that is coming from the DMA.
static uint32_t adc_fifo[256];
#define ADC_FIFO_HALFSIZE (sizeof(adc_fifo) / sizeof(uint32_t) / 2)

struct signal_buffer_t signal_buffer = {0, 0};

enum menu1_entry {ENTRY_MEMORY_DUMP = 4, 
                 ENTRY_NORMAL_SCROLL = 0, 
                 ENTRY_TRANSIENT_SCROLL = 1};
                 
enum scroll_mode_enum {NORMAL_SCROLL, TRANSIENT_SCROLL};

scroll_mode_enum scroll_mode;

// This function is the hotspot of the whole capture process.
// It compares the samples until it finds an edge.
const uint32_t * __attribute__((optimize("O3")))
find_edge(const uint32_t *data, const uint32_t *end, const uint32_t mask, const uint32_t old)
{
    // Get to a 4xsizeof(int) boundary
    while (((uint32_t)data & 0x0F) != ((uint32_t)adc_fifo & 0x0F))
    {
        if ((*data & mask) != old) return data;
        data++;
    }
    
    while (data < end)
    {
        if ((*data & mask) != old) return data;
        data++;
        if ((*data & mask) != old) return data;
        data++;
        if ((*data & mask) != old) return data;
        data++;
        if ((*data & mask) != old) return data;
        data++;
    }
    
    return end;
}

static void
process_samples(const uint32_t *data) 
{
    static uint32_t old = 0;
    static signaltime_t count = 0;
    
    // Compare the highest bit of each channel and the digital inputs.
    const uint32_t mask = 0x00038080;
    
    const uint32_t *end = data + ADC_FIFO_HALFSIZE;
    for(;;)
    {
        const uint32_t *start = data;
        data = find_edge(data, end, mask, old);
        
        // Update count
        count += data - start;
        
        if (data == end)
            break; // All done.
        
        // Just a sanity-check
        if (*data & 0xFF000000)
        {
            crash_with_message("Lost the H_L sync", __builtin_return_address(0));
            while(1);
        }
        
        // We may need up to 10 bytes of space in the buffer
        if (sizeof(signal_buffer.storage) < signal_buffer.bytes + 10)
        {
            // Buffer is full
            NVIC_DisableIRQ(DMA1_Channel4_IRQn);
            return;
        }

        // Write the value as base-128 varint (google protobuf-style)
        uint64_t value_to_write = (count << 4) + signal_buffer.last_value;
        uint8_t *p = signal_buffer.storage + signal_buffer.bytes;
        int i = 0;
        while (value_to_write)
        {
            p[i] = (value_to_write & 0x7F) | 0x80;
            value_to_write >>= 7;
            i++;
        }
        p[i - 1] &= 0x7F; // Unset top bit on last byte
        signal_buffer.bytes += i;

        // Prepare for seeking the next edge
        old = (*data & mask);
        count = 0;
        
        signal_buffer.last_value = 0;
        if (*data & 0x00000080) signal_buffer.last_value |= 1; // Channel A
        if (*data & 0x00008000) signal_buffer.last_value |= 2; // Channel B
        if (*data & 0x00010000) signal_buffer.last_value |= 4; // Channel C
        if (*data & 0x00020000) signal_buffer.last_value |= 8; // Channel D
    }
    
    signal_buffer.last_duration = count;
}

void __irq__ DMA1_Channel4_IRQHandler()
{
    if (DMA1->ISR & DMA_ISR_TEIF4)
    {
        crash_with_message("Oh noes: DMA channel 4 transfer error!",
            __builtin_return_address(0)
        );
        while(1);
    }
    else if (DMA1->ISR & DMA_ISR_HTIF4)
    {
        process_samples(&adc_fifo[0]);
        DMA1->IFCR = DMA_IFCR_CHTIF4;
        if (DMA1->ISR & DMA_ISR_TCIF4)
        {
            crash_with_message("Oh noes: ADC fifo overflow in HTIF", __builtin_return_address(0));
            while(1);
        }
    }
    else if (DMA1->ISR & DMA_ISR_TCIF4)
    {
        process_samples(&adc_fifo[ADC_FIFO_HALFSIZE]);
        DMA1->IFCR = DMA_IFCR_CTCIF4;
        if (DMA1->ISR & DMA_ISR_HTIF4)
        {
            crash_with_message("Oh noes: ADC fifo overflow in TCIF", __builtin_return_address(0));
            while(1);
        }
    }
}

void start_capture()
{
    // Samplerate is 500kHz, two TMR1 cycles per sample -> PSC = 12 -1, ARR = 6 - 1
    // Channel 2: Trigger DMA Ch3 to write H_L bit
    // Channel 4: Trigger DMA Ch4 to read data to memory
    //
    // TMR cycle:    0  1  2  3  4  5  0  1  2  3  4  5 0
    // MCO output:  _|^^^^^^^^^^^^^^^^^|________________|^
    // H_L:         _|^^^^^^^^^^^^^^^^^|________________|^
    // DMA sample:         ^ read ch A&B     ^ read ch C&D
    TIM1->CR1 = 0; // Turn off TIM1 until we are ready
    TIM1->CR2 = 0;
    TIM1->CNT = 0;
    TIM1->SR = 0;
    TIM1->PSC = 11;
    TIM1->ARR = 5;
    TIM1->CCMR1 = 0x0000; // CC2 time base
    TIM1->CCMR2 = 0x0000; // CC4 time base
    TIM1->DIER = TIM_DIER_CC2DE | TIM_DIER_CC4DE;
    TIM1->CCR1 = 0;
    TIM1->CCR2 = 0;
    TIM1->CCR4 = 2;
    
    // Reset the signal buffer
    signal_buffer.last_duration = 0;
    signal_buffer.bytes = 0;
    
    // DMA1 channel 3: copy data from hl_set to GPIOC->BSRR
    // Priority: very high
    // MSIZE = PSIZE = 32 bits
    // MINC enabled, CIRC mode enabled
    // Direction: read from memory
    // No interrupts
    DMA1_Channel3->CCR = 0;
    DMA1_Channel3->CNDTR = 2;
    DMA1_Channel3->CPAR = (uint32_t)&GPIOC->BSRR;
    DMA1_Channel3->CMAR = (uint32_t)hl_set;
    DMA1_Channel3->CCR = 0x3AB1;
    GPIOC->BSRR = hl_set[1];
    
    // DMA1 channel 4: copy data from FPGA to adc_fifo.
    // Priority: very high
    // MSIZE = PSIZE = 16 bits
    // MINC enabled, CIRC mode enabled
    // Direction: read from peripheral
    // Half- and Full-transfer interrupts, plus error interrupt
    DMA1_Channel4->CCR = 0;
    DMA1_Channel4->CNDTR = sizeof(adc_fifo) / 2;
    DMA1_Channel4->CPAR = 0x64000000; // FPGA memory-mapped address
    DMA1_Channel4->CMAR = (uint32_t)adc_fifo;
    DMA1_Channel4->CCR = 0x35AF;
    
    // Reduce the wait states of the FPGA & LCD interface
    FSMC_BTR1 = 0x10100110;
    FSMC_BTR2 = 0x10100110;
    FSMC_BCR1 |= FSMC_BCR1_CBURSTRW;
    
    // Clear any pending interrupts for ch 4
    DMA1->IFCR = 0x0000F000;
    
    // Enable ch 4 interrupt
    NVIC_EnableIRQ(DMA1_Channel4_IRQn);
    NVIC_SetPriority(DMA1_Channel4_IRQn, 0); // Highest priority
    
    // Now, lets go!
    TIM1->CR1 |= TIM_CR1_CEN;
}

void draw_screen(const std::vector<Drawable*> &objs, int startx, int endx)
{
    const int screenheight = 240;
    uint16_t buffer1[screenheight];
    uint16_t buffer2[screenheight];
    
    for (Drawable *d: objs)
    {
        d->Prepare(startx, endx);
    }
    
    lcd_set_location(startx, 0);
    for (int x = startx; x < endx; x++)
    {
        uint16_t *buffer = (x % 2) ? buffer1 : buffer2;
        memset(buffer, 0, screenheight * 2);
        
        for (Drawable *d: objs)
        {
            d->Draw(buffer, screenheight, x);
        }
        
        lcd_write_dma(buffer, screenheight);
    }
}

#include "gpio.h"
DECLARE_GPIO(usart1_tx, GPIOA, 9);
DECLARE_GPIO(usart1_rx, GPIOA, 10);

void show_status(const std::vector<Drawable*> &screenobjs, TextDrawable &statustext, const char *fmt, ...)
{
    char buffer[50];
    va_list va;
    va_start(va, fmt);
    int rv = vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);
    
    statustext.set_text(buffer);
    
    draw_screen(screenobjs, 0, 400);
}

void menu_click(int index, MenuDrawable *menu)
{
    if (index == ENTRY_MEMORY_DUMP)  //do a memory dump
    {
        crash_with_message("User-initiated memory dump",
                            __builtin_return_address(0));
    }
    else if (index == ENTRY_NORMAL_SCROLL)
    {
        menu->setColor(0, WHITE);
        menu->setColor(1, GREY);
        scroll_mode = NORMAL_SCROLL;
    }
    else if (index == ENTRY_TRANSIENT_SCROLL)
    {
        menu->setColor(0, GREY);
        menu->setColor(1, WHITE);
        scroll_mode = TRANSIENT_SCROLL;
    }
}

int main(void)
{   
    __Set(BEEP_VOLUME, 0);
    __Display_Str(80, 50, RGB565RGB(0,255,0), 0, (u8*)"Logic Analyzer (c) 2012 jpa");
    
    lcd_init();
    lcd_printf(80, 34, RGB565RGB(0,255,0), 0, "LCD TYPE %08lx", LCD_TYPE);
    
    // USART1 8N1 115200bps debug port
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    USART1->BRR = ((72000000 / (16 * 115200)) << 4) | 1;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
    gpio_usart1_tx_mode(GPIO_AFOUT_10);
    gpio_usart1_rx_mode(GPIO_HIGHZ_INPUT);
    
    __Set(ADC_CTRL, EN);       
    __Set(ADC_MODE, SEPARATE);               

    __Set(CH_A_COUPLE, DC);
    __Set(CH_A_RANGE, ADC_500mV);
    
    __Set(CH_B_COUPLE, DC);
    __Set(CH_B_RANGE, ADC_500mV);
    
    __Set(TRIGG_MODE, UNCONDITION);
    __Set(T_BASE_PSC, 0);
    __Set(T_BASE_ARR, 1); // MCO as sysclock/2
    __Set(CH_A_OFFSET, 0);
    __Set(CH_B_OFFSET, 0);
    __Set_Param(FPGA_SP_PERCNT_L, 0);
    __Set_Param(FPGA_SP_PERCNT_H, 0);
    
    __Read_FIFO();
    __Read_FIFO();
    
    while (~__Get(KEY_STATUS) & ALL_KEYS);
    get_keys(ANY_KEY);
    delay_ms(500); // Wait for ADC to settle
    
    start_capture();
    
    DSOSignalStream stream(&signal_buffer);
    XPosHandler xpos(400, stream);
    
    //init gui
    std::vector<Drawable*> screenobjs;
    Window graphwindow(64, 0, 400, 240);
    screenobjs.push_back(&graphwindow);

    Grid grid(stream, &xpos);
    grid.color = RGB565RGB(63, 63, 63);
    grid.y0 = 60;
    grid.y1 = 170;
    graphwindow.items.push_back(&grid);
    
    uint16_t colors[4] = {0xFFE0, 0x07FF, 0xF81F, 0x07E0};
    char names[4][6] = {"CH(A)", "CH(B)", "CH(C)", "CH(D)"};
    for (int i = 0; i < 4; i++)
    {
        SignalGraph* graph = new SignalGraph(stream, &xpos, i);
        graph->y0 = 150 - i * 30;
        graph->color = colors[i];
        
        graphwindow.items.push_back(graph);
        
        int middle_y = graph->y0 + graph->height / 2;
        TextDrawable* text = new TextDrawable(50, middle_y, names[i]);
        text->valign = TextDrawable::MIDDLE;
        text->halign = TextDrawable::RIGHT;
        text->color = colors[i];
        screenobjs.push_back(text);
    }
    
    BreakLines breaklines(&xpos, 500000);
    breaklines.linecolor = RGB565RGB(127, 127, 127);
    breaklines.textcolor = RGB565RGB(127, 127, 127);
    breaklines.y0 = 50;
    breaklines.y1 = 180;
    graphwindow.items.push_back(&breaklines);
    
    TimeMeasure timemeasure(&xpos);
    timemeasure.linecolor = 0xFF00;
    graphwindow.items.push_back(&timemeasure);
    
    Cursor cursor(&xpos);
    cursor.linecolor = 0x00FF;
    graphwindow.items.push_back(&cursor);
    
    TextDrawable button1txt(0, 240, " CLEAR ");
    button1txt.invert = true;
    screenobjs.push_back(&button1txt);
    
    TextDrawable button2txt(65, 240, " SAVE ");
    button2txt.invert = true;
    screenobjs.push_back(&button2txt);
    
    TextDrawable button3txt(130, 240, " BMP ");
    button3txt.invert = true;
    screenobjs.push_back(&button3txt);
    
    TextDrawable button4txt(180, 240, " SETTINGS ");
    button4txt.invert = true;
    screenobjs.push_back(&button4txt);
    
    MenuDrawable menu1(180,116,5);
    menu1.setText(0,"Normal Scroll");
    menu1.setColor(0, WHITE);
    menu1.setText(1,"Trans. Scroll");
    menu1.setColor(1, GREY);
    menu1.setSeparator(1,true);
    menu1.setText(2,"Selected");
    menu1.setColor(2, WHITE);
    menu1.setText(3,"Not Selected");
    menu1.setColor(3, GREY);
    menu1.setSeparator(3, true);
    menu1.setText(4,"Memory Dump");
    menu1.index = 2;
    menu1.visible = false;
    screenobjs.push_back(&menu1);
        
    TextDrawable statustext(390, 0, "");
    statustext.halign = TextDrawable::RIGHT;
    statustext.valign = TextDrawable::BOTTOM;
    screenobjs.push_back(&statustext);
    
    scroll_mode = NORMAL_SCROLL;
    
    while(1) {
        xpos.set_zoom(xpos.get_zoom());
        
        size_t free_bytes, largest_block;
        get_malloc_memory_status(&free_bytes, &largest_block);
        
        // Show_status also redraws the screen.
        // Yeah yeah, I know it's ugly.
        show_status(screenobjs, statustext,
                    "Position: %u us  Buffer: %2ld %%  RAM: %4d B",
                 (unsigned)(xpos.get_xpos() * 1000000 / DSOSignalStream::frequency),
                    div_round(signal_buffer.bytes * 100, sizeof(signal_buffer.storage)),
                 free_bytes);
        
        uint32_t start = get_time();
        uint32_t keys;
        while (!(keys = get_keys(ANY_KEY)) && (get_time() - start) < 100);
        
        if (keys & BUTTON1)
        {
            start_capture();
            xpos.set_xpos(0);
        }
        
        if (keys & BUTTON2)
        {
            stream.seek(0);
            
            char *name = select_filename("WAVES%03d.VCD");
            show_status(screenobjs, statustext, "Writing data to %s ", name);
            
            _fopen_wr(name);
            _fprintf("$version DSO Quad Logic Analyzer $end\n");
            _fprintf("$timescale 2us $end\n");
            _fprintf("$scope module logic $end\n");
            _fprintf("$var wire 1 A ChannelA $end\n");
            _fprintf("$var wire 1 B ChannelB $end\n");
            _fprintf("$var wire 1 C ChannelC $end\n");
            _fprintf("$var wire 1 D ChannelD $end\n");
            _fprintf("$upscope $end\n");
            _fprintf("$enddefinitions $end\n");
            _fprintf("$dumpvars 0A 0B 0C 0D $end\n");
            
            SignalEvent event;
            while (stream.read_forwards(event))
            {
                _fprintf("#%lu %dA %dB %dC %dD\n",
                         (uint32_t)event.start,
                         !!(event.levels & 1), !!(event.levels & 2),
                         !!(event.levels & 4), !!(event.levels & 8)
                );
            }
            
            _fprintf("#%lu\n", (uint32_t)event.end);
            
            if (_fclose())
            {
                show_status(screenobjs, statustext, "%s successfully written", name);
            }
            else
            {
                show_status(screenobjs, statustext, "Failed to write file.");
            }
            
            delay_ms(3000);
        }
        
        if (keys & BUTTON3)
        {
            char *name = select_filename("LOGIC%03d.BMP");
            show_status(screenobjs, statustext, "Writing screenshot to %s ", name);
            
            if (write_bitmap(name))
            {
                show_status(screenobjs, statustext, "Wrote %s successfully!", name);
            }
            else
            {
                show_status(screenobjs, statustext, "Bitmap write failed.");
            }
            
            delay_ms(3000);
        }
        
        if (keys & BUTTON4)
        {
           // toggle the menu
            menu1.visible = !menu1.visible;
        }
        
        if (keys & SCROLL2_LEFT)
        {
            if (menu1.visible)
            {
                menu1.previous();
            }
            else    //scroll
            {
                if (scroll_mode == NORMAL_SCROLL)
                {
                    xpos.move_xpos(-scroller_speed());
                } 
                else if (scroll_mode == TRANSIENT_SCROLL)
                {
                    SignalEvent event;
                    int offset;
                    signaltime_t center_time = xpos.get_xpos();
                    stream.seek(center_time);
                    stream.read_backwards(event);
                    xpos.set_xpos(event.start);
                }
            }
        }
        
        if (keys & SCROLL2_RIGHT)
        {
            if (menu1.visible)
            {
                menu1.next();
            }
            else    //scroll
            {
                if (scroll_mode == NORMAL_SCROLL)
                {
                    xpos.move_xpos(scroller_speed());
                }
                else if (scroll_mode == TRANSIENT_SCROLL)
                {
                    SignalEvent event;
                    int offset;
                    signaltime_t center_time = xpos.get_xpos();
                    stream.seek(center_time);
                    stream.read_forwards(event);
                    xpos.set_xpos(event.end);
                }
            }
        }
        
        if (keys & SCROLL2_PRESS)
        {
            if (menu1.visible)
            {
                menu_click(menu1.index, &menu1);
            }
            else
            {
                timemeasure.Click();
            }
         }
        
        int zoom = xpos.get_zoom();
        if ((keys & SCROLL1_LEFT) && zoom > -30)
            xpos.set_zoom(zoom - 1);
        
        if ((keys & SCROLL1_RIGHT) && zoom < 3)
            xpos.set_zoom(zoom + 1);
    }
    
    return 0;
}

