#include "epd.h"

#include <stdbool.h>
#include <string.h>

#include "asmUtil.h"
#include "board.h"
#include "cpu.h"
#include "font.h"
#include "lut.h"
#include "printf.h"
#include "screen.h"
#include "sleep.h"
#include "spi.h"
#include "timer.h"

#define CMD_DRV_OUTPUT_CTRL 0x01
#define CMD_SOFT_START_CTRL 0x0C
#define CMD_ENTER_SLEEP 0x10
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SOFT_RESET 0x12
#define CMD_SOFT_RESET2 0x13
#define CMD_SETUP_VOLT_DETECT 0x15
#define CMD_TEMP_SENSOR_CONTROL 0x18
#define CMD_ACTIVATION 0x20
#define CMD_DISP_UPDATE_CTRL 0x21
#define CMD_DISP_UPDATE_CTRL2 0x22
#define CMD_WRITE_FB_BW 0x24
#define CMD_WRITE_FB_RED 0x26
#define CMD_UNKNOWN_1 0x2B
#define CMD_LOAD_OTP_LUT 0x31
#define CMD_WRITE_LUT 0x32
#define CMD_BORDER_WAVEFORM_CTRL 0x3C
#define CMD_WINDOW_X_SIZE 0x44
#define CMD_WINDOW_Y_SIZE 0x45
#define CMD_WRITE_PATTERN_RED 0x46
#define CMD_WRITE_PATTERN_BW 0x47
#define CMD_XSTART_POS 0x4E
#define CMD_YSTART_POS 0x4F
#define CMD_ANALOG_BLK_CTRL 0x74
#define CMD_DIGITAL_BLK_CTRL 0x7E

#define SCREEN_CMD_CLOCK_ON 0x80
#define SCREEN_CMD_CLOCK_OFF 0x01
#define SCREEN_CMD_ANALOG_ON 0x40
#define SCREEN_CMD_ANALOG_OFF 0x02
#define SCREEN_CMD_LATCH_TEMPERATURE_VAL 0x20
#define SCREEN_CMD_LOAD_LUT 0x10
#define SCREEN_CMD_USE_MODE_2 0x08  // modified commands 0x10 and 0x04
#define SCREEN_CMD_REFRESH 0xC7

#define commandEnd() \
    do {             \
        P1_7 = 1;    \
    } while (0)

#define markCommand() \
    do {              \
        P2_2 = 0;     \
    } while (0)

#define markData() \
    do {           \
        P2_2 = 1;  \
    } while (0)

static uint8_t __xdata currentLUT = 0x00;  // Current selected LUT
static bool __idata epdPr = false;         // wheter or not we copy the pr("") output to the EPD
static uint8_t __xdata epdCharSize = 1;    // character size, 1 or 2 (doubled)
static bool __xdata directionY = true;     // print direction, X or Y (true)
static uint8_t __xdata rbuffer[32];        // used to rotate bits around
static uint16_t __xdata fontCurXpos = 0;   // current X value where working with
static bool __xdata isInited = false;

#pragma callee_saves epdBusySleep
#pragma callee_saves epdBusyWait
static void epdBusySleep(uint32_t timeout) {
    uint8_t tmp_P2FUNC = P2FUNC;
    uint8_t tmp_P2DIR = P2DIR;
    uint8_t tmp_P2PULL = P2PULL;
    uint8_t tmp_P2LVLSEL = P2LVLSEL;
    P2FUNC &= 0xfd;
    P2DIR |= 2;
    P2PULL |= 2;
    P2LVLSEL |= 2;

    P2CHSTA &= 0xfd;
    P2INTEN |= 2;
    P2CHSTA &= 0xfd;
    sleepForMsec(timeout);
    P2CHSTA &= 0xfd;
    P2INTEN &= 0xfd;

    P2FUNC = tmp_P2FUNC;
    P2DIR = tmp_P2DIR;
    P2PULL = tmp_P2PULL;
    P2LVLSEL = tmp_P2LVLSEL;
    eepromPrvDeselect();
}
static void epdBusyWait(uint32_t timeout) {
    uint32_t __xdata start = timerGet();

    while (timerGet() - start < timeout) {
        if (!P2_1)
            return;
    }
    pr("screen timeout %lu ticks :(\n", timerGet() - start);
    while (1)
        ;
}
static void commandReadBegin(uint8_t cmd) {
    epdSelect();
    markCommand();
    spiByte(cmd);  // dump LUT

    P0DIR = (P0DIR & ~(1 << 0)) | (1 << 1);
    P0 &= ~(1 << 0);
    P0FUNC &= ~((1 << 0) | (1 << 1));
    P2_2 = 1;
}
static void commandReadEnd() {
    // set up pins for spi (0.0,0.1,0.2)
    P0FUNC |= (1 << 0) | (1 << 1);
    epdDeselect();
}
#pragma callee_saves epdReadByte
static uint8_t epdReadByte() {
    uint8_t val = 0, i;

    for (i = 0; i < 8; i++) {
        P0_0 = 1;
        __asm__("nop\nnop\nnop\nnop\nnop\nnop\n");
        val <<= 1;
        if (P0_1)
            val++;
        P0_0 = 0;
        __asm__("nop\nnop\nnop\nnop\nnop\nnop\n");
    }

    return val;
}
static void shortCommand(uint8_t cmd) {
    epdSelect();
    markCommand();
    spiTXByte(cmd);
    epdDeselect();
}
static void shortCommand1(uint8_t cmd, uint8_t arg) {
    epdSelect();
    markCommand();
    spiTXByte(cmd);
    markData();
    spiTXByte(arg);
    epdDeselect();
}
static void shortCommand2(uint8_t cmd, uint8_t arg1, uint8_t arg2) {
    epdSelect();
    markCommand();
    spiTXByte(cmd);
    markData();
    spiTXByte(arg1);
    spiTXByte(arg2);
    epdDeselect();
}
static void commandBegin(uint8_t cmd) {
    epdSelect();
    markCommand();
    spiTXByte(cmd);
    markData();
}
static void epdReset() {
    timerDelay(TIMER_TICKS_PER_SECOND / 1000);
    P2_0 = 0;
    timerDelay(TIMER_TICKS_PER_SECOND / 1000);
    P2_0 = 1;
    timerDelay(TIMER_TICKS_PER_SECOND / 1000);

    shortCommand(CMD_SOFT_RESET);  // software reset
    timerDelay(TIMER_TICKS_PER_SECOND / 1000);
    shortCommand(CMD_SOFT_RESET2);
    timerDelay(TIMER_TICKS_PER_SECOND / 1000);
}
void epdEnterSleep() {
    P2_0 = 0;
    timerDelay(10);
    P2_0 = 1;
    timerDelay(50);
    shortCommand(CMD_SOFT_RESET2);
    epdBusyWait(133300);
    shortCommand1(CMD_ENTER_SLEEP, 0x03);
    isInited = false;
}
void epdSetup() {
    epdReset();
    currentLUT = 0;
    shortCommand1(CMD_ANALOG_BLK_CTRL, 0x54);
    shortCommand1(CMD_DIGITAL_BLK_CTRL, 0x3B);
    shortCommand2(CMD_UNKNOWN_1, 0x04, 0x63);

    commandBegin(CMD_SOFT_START_CTRL);
    epdSend(0x8f);
    epdSend(0x8f);
    epdSend(0x8f);
    epdSend(0x3f);
    commandEnd();

    commandBegin(CMD_DRV_OUTPUT_CTRL);
    epdSend((SCREEN_HEIGHT - 1) & 0xff);
    epdSend((SCREEN_HEIGHT - 1) >> 8);
    epdSend(0x00);
    commandEnd();

    // shortCommand1(CMD_DATA_ENTRY_MODE, 0x03);
    shortCommand1(CMD_BORDER_WAVEFORM_CTRL, 0xC0);
    shortCommand1(CMD_TEMP_SENSOR_CONTROL, 0x80);
    shortCommand1(CMD_DISP_UPDATE_CTRL2, 0xB1);  // mode 1 (i2C)
    // shortCommand1(CMD_DISP_UPDATE_CTRL2, 0xB9);  // mode 2?
    shortCommand(CMD_ACTIVATION);
    epdBusyWait(1333000UL);
    isInited = true;
}
static uint8_t epdGetStatus() {
    uint8_t sta;
    commandReadBegin(0x2F);
    sta = epdReadByte();
    commandReadEnd();
    return sta;
}
uint16_t epdGetBattery(void) {
    uint16_t voltage = 2600;

    if (!isInited)
        epdReset();
    uint8_t val;

    shortCommand1(CMD_DISP_UPDATE_CTRL2, SCREEN_CMD_CLOCK_ON | SCREEN_CMD_ANALOG_ON);
    shortCommand(CMD_ACTIVATION);
    epdBusyWait(133300);

    for (val = 3; val < 8; val++) {
        shortCommand1(CMD_SETUP_VOLT_DETECT, val);
        epdBusyWait(133300);
        if (epdGetStatus() & 0x10) {  // set if voltage is less than threshold ( == 1.9 + val / 10)
            voltage = 1850 + mathPrvMul8x8(val, 100);
            break;
        }
    }

    shortCommand1(CMD_DISP_UPDATE_CTRL2, 0xB1);
    shortCommand(CMD_ACTIVATION);
    epdBusyWait(133300);

    if (!isInited)
        epdEnterSleep();
    return voltage;
}

void selectLUT(uint8_t lut) {
    if (lut == currentLUT) return;
    // lut = 1;
    switch (lut) {
        case 0:
            shortCommand1(CMD_DISP_UPDATE_CTRL2, 0xB1);  // mode 1?
            shortCommand(CMD_ACTIVATION);
            epdBusyWait(1333000UL);
            break;
        case 1:
            commandBegin(CMD_WRITE_LUT);
            for (uint8_t i = 0; i < 70; i++)
                epdSend(lutorig[i]);
            commandEnd();
            break;
    }
    currentLUT = lut;
}

void setWindowX(uint16_t start, uint16_t end) {
    shortCommand2(CMD_WINDOW_X_SIZE, start / 8, end / 8 - 1);
}
void setWindowY(uint16_t start, uint16_t end) {
    commandBegin(CMD_WINDOW_Y_SIZE);
    epdSend((start)&0xff);
    epdSend((start) >> 8);
    epdSend((end - 1) & 0xff);
    epdSend((end - 1) >> 8);
    commandEnd();
}
void setPosXY(uint16_t x, uint16_t y) {
    shortCommand1(CMD_XSTART_POS, (uint8_t)(x / 8));
    commandBegin(CMD_YSTART_POS);
    epdSend((y)&0xff);
    epdSend((y) >> 8);
    commandEnd();
}
void setColorMode(uint8_t red, uint8_t bw) {
    shortCommand1(CMD_DISP_UPDATE_CTRL, (red << 4) | bw);
}
void fillWindowWithPattern(bool color) {
    if (color == EPD_COLOR_RED) {
        shortCommand1(CMD_WRITE_PATTERN_RED, 0x00);
    } else {
        shortCommand1(CMD_WRITE_PATTERN_BW, 0x00);
    }
}
void clearWindow(bool color) {
    if (color == EPD_COLOR_RED) {
        shortCommand1(CMD_WRITE_PATTERN_RED, 0x66);
    } else {
        shortCommand1(CMD_WRITE_PATTERN_BW, 0x66);
    }
}
void clearScreen() {
    setWindowX(0, SCREEN_WIDTH);
    setWindowY(0, SCREEN_HEIGHT);
    setPosXY(0, 0);
    shortCommand1(CMD_WRITE_PATTERN_BW, 0x66);
    epdBusyWait(133300UL);
    shortCommand1(CMD_WRITE_PATTERN_RED, 0x66);
    epdBusyWait(133300UL);
}
void draw() {
    shortCommand1(0x22, 0xCF);
    // shortCommand1(0x22, SCREEN_CMD_REFRESH);
    shortCommand(0x20);
    epdBusyWait(TIMER_TICKS_PER_SECOND * 120);
}
void drawNoWait() {
    shortCommand1(0x22, 0xCF);
    // shortCommand1(0x22, SCREEN_CMD_REFRESH);
    shortCommand(0x20);
}
void drawLineHorizontal(bool red, uint16_t y, uint8_t width) {
    setWindowX(0, SCREEN_WIDTH);
    setWindowY(y, y + width);
    if (red) {
        shortCommand1(CMD_WRITE_PATTERN_RED, 0xE6);
    } else {
        shortCommand1(CMD_WRITE_PATTERN_BW, 0xE6);
    }
    epdBusyWait(133300UL);
}
void beginFullscreenImage() {
    setColorMode(EPD_MODE_NORMAL, EPD_MODE_INVERT);
    setWindowX(0, SCREEN_WIDTH);
    setWindowY(0, SCREEN_HEIGHT);
    shortCommand1(CMD_DATA_ENTRY_MODE, 3);
    setPosXY(0, 0);
}
void beginWriteFramebuffer(bool color) {
    if (color == EPD_COLOR_RED) {
        commandBegin(CMD_WRITE_FB_RED);
    } else {
        commandBegin(CMD_WRITE_FB_BW);
    }
    epdDeselect();
}
void endWriteFramebuffer() {
    commandEnd();
}

// stuff for printing text
static void pushXFontBytesToEPD(uint8_t byte1, uint8_t byte2) {
    if (epdCharSize == 1) {
        uint8_t offset = 7 - (fontCurXpos % 8);
        for (uint8_t c = 0; c < 8; c++) {
            if (byte1 & (1 << c)) rbuffer[c] |= (1 << offset);
        }
        for (uint8_t c = 0; c < 8; c++) {
            if (byte2 & (1 << c)) rbuffer[8 + c] |= (1 << offset);
        }
        fontCurXpos++;
    } else {
        uint8_t offset = 6 - (fontCurXpos % 8);
        // double font size
        for (uint8_t c = 0; c < 8; c++) {
            if (byte1 & (1 << c)) {
                rbuffer[c * 2] |= (3 << offset);
                rbuffer[(c * 2) + 1] |= (3 << offset);
            }
        }
        for (uint8_t c = 0; c < 8; c++) {
            if (byte2 & (1 << c)) {
                rbuffer[(c * 2) + 16] |= (3 << offset);
                rbuffer[(c * 2) + 17] |= (3 << offset);
            }
        }
        fontCurXpos += 2;
    }
    if (fontCurXpos % 8 == 0) {
        // next byte, flush current byte to EPD

        for (uint8_t i = 0; i < (16 * epdCharSize); i++) {
            epdSend(rbuffer[i]);
        }
        memset(rbuffer, 0, 32);
    }
}
static void pushYFontBytesToEPD(uint8_t byte1, uint8_t byte2) {
    if (epdCharSize == 2) {
        for (uint8_t j = 0; j < 2; j++) {
            uint8_t c = 0;
            for (uint8_t i = 7; i != 255; i--) {
                if (byte1 & (1 << i)) c |= (0x03 << ((i % 4) * 2));
                if ((i % 4) == 0) {
                    epdSend(c);
                    c = 0;
                }
            }
            for (uint8_t i = 7; i != 255; i--) {
                if (byte2 & (1 << i)) c |= (0x03 << ((i % 4) * 2));
                if ((i % 4) == 0) {
                    epdSend(c);
                    c = 0;
                }
            }
        }
    } else {
        epdSend(byte1);
        epdSend(byte2);
    }
}
void writeCharEPD(uint8_t c) {
    if (!epdPr) {
        return;
    }
    // Writes a single character to the framebuffer
    bool empty = true;
    for (uint8_t i = 0; i < 20; i++) {
        if (font[c][i]) empty = false;
    }
    if (empty) {
        for (uint8_t i = 0; i < 8; i++) {
            if (directionY) {
                pushYFontBytesToEPD(0x00, 0x00);
            } else {
                pushXFontBytesToEPD(0x00, 0x00);
            }
        }
        return;
    }

    uint8_t begin = 0;
    while (font[c][begin] == 0x00 && font[c][begin + 1] == 0x00) {
        begin += 2;
    }

    uint8_t end = 20;
    while (font[c][end - 1] == 0x00 && font[c][end - 2] == 0x00) {
        end -= 2;
    }

    for (uint8_t pos = begin; pos < end; pos += 2) {
        if (directionY) {
            pushYFontBytesToEPD(font[c][pos + 1], font[c][pos]);
        } else {
            pushXFontBytesToEPD(font[c][pos], font[c][pos + 1]);
        }
    }

    // spacing between characters
    if (directionY) {
        pushYFontBytesToEPD(0x00, 0x00);
    } else {
        pushXFontBytesToEPD(0x00, 0x00);
    }
}
// Print text to the EPD. Origin is top-left
void epdPrintBegin(uint16_t x, uint16_t y, bool direction, bool fontsize, bool color) {
    directionY = direction;
    epdCharSize = 1 + fontsize;
    if (directionY) {
        if (epdCharSize == 2) {
            setWindowX(x - 32, x);
            setPosXY(x - 32, y);

        } else {
            setWindowX(x - 16, x);
            setPosXY(x - 16, y);
        }
        setWindowY(y, SCREEN_HEIGHT);
        shortCommand1(CMD_DATA_ENTRY_MODE, 3);
    } else {
        if (epdCharSize == 2) {
            x /= 2;
            x *= 2;
            setWindowY(y, y + 32);
        } else {
            setWindowY(y, y + 16);
        }
        setPosXY(x, y);
        fontCurXpos = x;
        setWindowX(x, SCREEN_WIDTH);
        shortCommand1(CMD_DATA_ENTRY_MODE, 7);
        memset(rbuffer, 0, 32);
    }

    epdPr = true;
    if (color) {
        commandBegin(CMD_WRITE_FB_RED);
    } else {
        commandBegin(CMD_WRITE_FB_BW);
    }
}
void epdPrintEnd() {
    if (!directionY && ((fontCurXpos % 8) != 0)) {
        for (uint8_t i = 0; i < (16 * epdCharSize); i++) {
            epdSend(rbuffer[i]);
        }
    }
    commandEnd();
    epdPr = false;
}

extern uint8_t* __xdata tempBuffer;
extern void dump(uint8_t* __xdata a, uint16_t __xdata l);

void loadFixedTempLUT() {
    shortCommand1(0x18, 0x48);
    shortCommand2(0x1A, 0x05, 0x00);             // < temp register
    shortCommand1(CMD_DISP_UPDATE_CTRL2, 0xB1);  // mode 1 (i2C)
    shortCommand(CMD_ACTIVATION);
    epdBusyWait(1333000UL);
}
static uint8_t readLut() {
    uint8_t sta = 0;

    commandReadBegin(0x33);

    uint16_t checksum = 0;
    uint16_t ident = 0;
    uint16_t shortl = 0;
    for (uint16_t c = 0; c < 76; c++) {
        sta = epdReadByte();
        checksum += sta;
        if (c < 70) ident += sta;
        if (c < 14) shortl += sta;
        tempBuffer[c] = sta;
    }
    pr("ident=%04X checksum=%04X shortl=%04X\n", ident, checksum, shortl);
    commandReadEnd();
    dump(tempBuffer, 96);

    return sta;
}