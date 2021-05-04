/*
Copyright 2021 Accutron Instruments Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include <python3.7/Python.h>
#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <unistd.h>
#include <stdio.h>

// global array of register data
uint16_t chip_regs[16];

#define I2C_ADDRESS 0x10

// Si4703 Register Addresses
#define DEVICEID 0x00
#define CHIPID 0x01
#define POWERCFG 0x02
#define CHANNEL 0x03
#define SYSCFG1 0x04
#define SYSCFG2 0x05
#define STATUSRSSI 0x0A
#define READCHAN 0x0B
#define RDSA 0x0C
#define RDSB 0x0D
#define RDSC 0x0E
#define RDSD 0x0F

// POWERCFG Register Bits
#define SMUTE_BIT 15
#define DMUTE_BIT 14
#define SEEKMODE_BIT 10
#define SEEKUP_BIT 9
#define SEEK_BIT 8
// SYSCFG1 Register Bits
#define RDS_BIT 12
#define DE_BIT 11
// CHANNEL Register Bits
#define TUNE_BIT 15
//Register 0x05 - SYSCONFIG2
#define SPACE1_BIT 5
#define SPACE0_BIT 4
//Register 0x0A - STATUSRSSI
#define RDSR_BIT 15
#define STC_BIT 14
#define SFBL_BIT 13
#define AFCRL_BIT 12
#define RDSS_BIT 11
#define STEREO_BIT 8

// RDS B Register
#define GROUP_MASK 0xF000
#define GROUP_OFFSET_BITS 12
#define SUBGROUP_BIT 11
#define TRAFFIC_BIT 10
#define MUSIC_BIT 3
#define POSITION_MASK 0x7



static int I2C_FILE_DESCRIPTOR = -1;

static void read_chip_registers(int fd)
{
    //Si4703 begins reading from upper register of 0x0A and reads to 0x0F, then loops to 0x00.
    uint8_t buffer[32];
    int rc = read(fd, buffer, 32);

    //TODO: handle rc

    for (int i = 0x0A, j = 0;; ++i, j += 2)
    {
        if (i == 0x10)
            i = 0;
        chip_regs[i] = buffer[j] << 8;
        chip_regs[i] |= buffer[j + 1];
        if (i == 0x09)
            break;
    }
}

static void write_chip_registers(int fd)
{
    uint8_t write_buffer[12];
    // write sequence starts at address 0x02
    // only send the 0x02 - 0x07 control registers
    for (int reg = 0x02, i = 0; reg < 0x08; ++reg, i += 2)
    {
        write_buffer[i] = chip_regs[reg] >> 8;
        write_buffer[i + 1] = chip_regs[reg] & 0x00FF;
    }

    int rc = write(fd, write_buffer, sizeof(write_buffer));
    // TODO: Check rc
}

static int read_channel(int* stereo, int* rssi)
{
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    int channel = chip_regs[READCHAN] & 0x03FF; // Channel data is the lower 10 bits
    channel *= 2;
    channel += 875;
    *stereo = chip_regs[STATUSRSSI] & (1 << STEREO_BIT);
    *rssi = chip_regs[STATUSRSSI] & 0b01111111;
    return channel;
}

static void get_rds_data()
{
    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if (chip_regs[STATUSRSSI] & (1 << RDSR_BIT))
        {
            char oa_buffer[1024];
            char twoa_buffer[1024];

            // uint16_t reg_a = (chip_regs[RDSA] & 0xFF00) >> 8;
            // reg_a |= (chip_regs[RDSA] & 0x00FF);

            // uint16_t reg_b = (chip_regs[RDSB] & 0xFF00) >> 8;
            // reg_b |= (chip_regs[RDSB] & 0x00FF);

            // uint16_t reg_c = (chip_regs[RDSC] & 0xFF00) >> 8;
            // reg_c |= (chip_regs[RDSC] & 0x00FF);

            // uint16_t reg_d = (chip_regs[RDSD] & 0xFF00) >> 8;
            // reg_d |= (chip_regs[RDSD] & 0x00FF);
            char Ah, Al, Bh, Bl, Ch, Cl, Dh, Dl;
            //Ah = (chip_regs[RDSA] & 0xFF00) >> 8;
            //Al = (chip_regs[RDSA] & 0x00FF);

            uint16_t b = chip_regs[RDSB];
            uint16_t group = (b & GROUP_MASK) >> GROUP_OFFSET_BITS;
            int subgroup = (b & (1 << SUBGROUP_BIT)) >> SUBGROUP_BIT;
            char sg = subgroup == 0 ? 'A' : 'B';

            int traffic = (b & (1 << TRAFFIC_BIT)) >> TRAFFIC_BIT;
            uint16_t pty = (b & 0b0000001111100000) >> 5;
            int music = (b & (1 << MUSIC_BIT)) >> MUSIC_BIT;
            int position = (b & POSITION_MASK);

            Dh = (chip_regs[RDSD] & 0xFF00) >> 8;
            Dl = (chip_regs[RDSD] & 0x00FF);

            if (group == 0 && subgroup == 0){
                oa_buffer[position] = Dh;
                oa_buffer[position + 1] = Dl;
                printf("Group 0A Buffer: %s\n", oa_buffer);
            }

            if (group == 2 && subgroup == 0){
                twoa_buffer[position] = Dh;
                twoa_buffer[position + 1] = Dl;
                printf("Group 2A Buffer: %s\n", twoa_buffer);
            }

            Ch = (chip_regs[RDSC] & 0xFF00) >> 8;
            Cl = (chip_regs[RDSC] & 0x00FF);

            Dh = (chip_regs[RDSD] & 0xFF00) >> 8;
            Dl = (chip_regs[RDSD] & 0x00FF);

            //printf("RDS Packet: %#x %#x %#x %#x\n", reg_a, reg_b, reg_c, reg_d);
            printf("RDS Packet: Raw %4x Group: %2x%c Traffic: %d PTY: %d Music/Speech: %d Position: %d %c %c\n", b, group, sg, traffic, pty, music, position, Dh, Dl);
            usleep(1000 * 40);
        }
        else
        {
            usleep(1000 * 20);
        }
    }
}

static int go_to_channel(int channel, int* stereo, int* rssi)
{
    channel *= 10;
    channel -= 8750;
    channel /= 20;

    read_chip_registers(I2C_FILE_DESCRIPTOR);
    chip_regs[CHANNEL] &= 0xFE00; // Clear all channel bits
    chip_regs[CHANNEL] |= channel;
    chip_regs[CHANNEL] |= (1 << TUNE_BIT);

    write_chip_registers(I2C_FILE_DESCRIPTOR);
    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if ((chip_regs[STATUSRSSI] & (1 << STC_BIT)) != 0)
            break;
    }
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    chip_regs[CHANNEL] &= ~(1 << TUNE_BIT); // it's up to us clear the tune bit when STC goes high
    write_chip_registers(I2C_FILE_DESCRIPTOR);
    // We're not done until STC goes back low
    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if ((chip_regs[STATUSRSSI] & (1 << STC_BIT)) == 0)
            break;
    }    
    int channel_data = read_channel(stereo, rssi);
    return channel_data;
}



static void initialize_chip()
{
    printf("Initializing...\n");
    wiringPiSetupGpio(); // Use BCM Numbers

    pinMode(5, OUTPUT);
    pinMode(2, OUTPUT);

    usleep(1000);

    digitalWrite(5, LOW);
    digitalWrite(2, LOW);
    usleep(100);
    digitalWrite(5, HIGH);
    usleep(100);

    pinModeAlt(2, 0x04);

    I2C_FILE_DESCRIPTOR = wiringPiI2CSetup(I2C_ADDRESS);
    if (I2C_FILE_DESCRIPTOR == -1)
    {
        //std::cout << "Could Not Setup I2C communication to address: " << I2C_ADDRESS << "\n";
        return PyLong_FromLong(1);
    }
    //std::cout << "I2C comms established with device address: " << I2C_ADDRESS << "\n";
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    //std::cout << "Chip ID: " << std::hex << chip_regs[0] << "\n";

    // ebable the oscillator
    chip_regs[0x07] = 0x8100;
    write_chip_registers(I2C_FILE_DESCRIPTOR);

    usleep(1000 * 500);

    // power up
    chip_regs[POWERCFG] = 0x4001;
    // enable RDS
    chip_regs[SYSCFG1] |= (1 << RDS_BIT);
    // ensure North American 200kHz spacing
    chip_regs[SYSCFG2] &= ~(1 << SPACE1_BIT | 1 << SPACE0_BIT);
    // clear volume bits
    chip_regs[SYSCFG2] &= 0xFFF0;
    // volume to some reasonable amount
    chip_regs[SYSCFG2] |= 0x0A;

    write_chip_registers(I2C_FILE_DESCRIPTOR);

    usleep(1000 * 110);

    //go_to_channel(fd, 1039);
    printf("Done\n");
}

static int seek(int dir, int* stereo, int* rssi){
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    // set seek wrap bit to disable
    chip_regs[POWERCFG] |= (1 << SEEKMODE_BIT);
    if (dir == 0) // seek down
    {
        // unset seek up in powercfg        
        chip_regs[POWERCFG] &= ~(1 << SEEKUP_BIT);
    }
    else
    {        
        chip_regs[POWERCFG] |= (1 << SEEKUP_BIT);
    }
    chip_regs[POWERCFG] |= (1 << SEEK_BIT);    
    write_chip_registers(I2C_FILE_DESCRIPTOR);
    usleep(1000);
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    printf("Current STC: %d\n", chip_regs[STATUSRSSI] & (1 << STC_BIT));    
    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if ((chip_regs[STATUSRSSI] & (1 << STC_BIT)) != 0)
            break; // seek is done
        printf("Trying Station: %d\n", read_channel(stereo, rssi));
        //printf("Waiting on STC High After seek...\n");
    }

    // see if we hit the band limit
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    int sfbl = chip_regs[STATUSRSSI] & (1 << SFBL_BIT);
    if (sfbl > 0)
    {
        printf("we have an sfbl problem\n");
    }
    // need to unset SEEK
    chip_regs[POWERCFG] &= ~(1 << SEEK_BIT);
    write_chip_registers(I2C_FILE_DESCRIPTOR);
    
    usleep(1000);
    read_chip_registers(I2C_FILE_DESCRIPTOR);    
    // wait on STC Clear
    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if ((chip_regs[STATUSRSSI] & (1 << STC_BIT)) == 0)
            break; // seek is done
        //printf("Waiting on STC Low After seek clear...\n");
    }
    int channel_data = read_channel(stereo, rssi);
    return channel_data;
}

static PyObject *method_initialize_chip(PyObject *self, PyObject *args)
{
    initialize_chip();
    return PyLong_FromLong(0);
}

PyObject *method_readRDS(PyObject *self, PyObject *args)
{
    get_rds_data();
    return PyLong_FromLong(0);
}

static PyObject *method_go_to_channel(PyObject *self, PyObject *args)
{
    if (I2C_FILE_DESCRIPTOR < 0)
    {
        return PyLong_FromLong(0);
    }

    int channel;
    if (!PyArg_ParseTuple(args, "i", &channel))
    {
        return PyLong_FromLong(0);
    }
    int stereo = 0;
    int rssi = 0;
    int channel_data = go_to_channel(channel, &stereo, &rssi);
    PyObject* result = PyTuple_New(3);    
    PyTuple_SetItem(result, 0, PyLong_FromLong(channel));
    PyTuple_SetItem(result, 1, PyBool_FromLong(stereo));
    PyTuple_SetItem(result, 2, PyLong_FromLong(rssi));
    return result;
}

PyObject *method_seek(PyObject *self, PyObject *args)
{
    int dir;
    if (!PyArg_ParseTuple(args, "i", &dir))
    {
        return PyBool_FromLong(0);
    }
    int stereo = 0;
    int rssi = 0;
    int channel = seek(dir, &stereo, &rssi);
    PyObject* result = PyTuple_New(3);
    PyTuple_SetItem(result, 0, PyLong_FromLong(channel));
    PyTuple_SetItem(result, 1, PyBool_FromLong(stereo));
    PyTuple_SetItem(result, 2, PyLong_FromLong(rssi));
    return result;
}

static PyObject *method_getChannel(PyObject *self, PyObject *args)
{
    int stereo = 0;
    int rssi = 0;
    PyObject* result = PyTuple_New(3);
    int channel = read_channel(&stereo, &rssi);
    PyTuple_SetItem(result, 0, PyLong_FromLong(channel));
    PyTuple_SetItem(result, 1, PyBool_FromLong(stereo));
    PyTuple_SetItem(result, 2, PyLong_FromLong(rssi));
    return result;
}

static PyMethodDef Si4703Methods[] = {
    {"initialize", method_initialize_chip, METH_VARARGS, "Initialize Chip"},
    {"goToChannel", method_go_to_channel, METH_VARARGS, "Tune to the specified channel."},
    {"seek", method_seek, METH_VARARGS, "Seek in given direction"},
    {"getChannel", method_getChannel, METH_VARARGS, "Get tHe currently tuned station."},
    {"readRDS", method_readRDS, METH_VARARGS, "Read RDS Data (if any)"},
    {NULL, NULL, 0, NULL}};

static PyModuleDef Si4703Module = {
    PyModuleDef_HEAD_INIT,
    "Si4703",
    "Python Interface for WiringPi driven Si4703 FM Tuner Chips",
    -1,
    Si4703Methods};

PyMODINIT_FUNC PyInit_Si4703(void)
{
    return PyModule_Create(&Si4703Module);
}
