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
#include <string.h>

static int rds_running = 0;

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
#define BLERA 0x0600

// RDS B Register
#define GROUP_MASK 0xF000
#define GROUP_OFFSET_BITS 12
#define SUBGROUP_BIT 11
#define TRAFFIC_BIT 10
#define MUSIC_BIT 3
#define POSITION_MASK 0x7
#define PTY_MASK 0x03E0
#define PTY_OFFSET 5

// READCHAN Register Bits
#define BLERB 0xC000
#define BLERC 0x3000
#define BLERD 0x0C00
#define RCHAN 0x03FF

typedef enum
{
    false,
    true
} bool_t;

typedef struct
{
    char program_service_name[9];
    char radio_text[64 + 1];
} rds_data_t;

typedef struct
{
    bool_t station_name;
    bool_t radio_text;
} rds_status_t;

static pthread_mutex_t rds_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t register_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t rds_thread_handle;

static int I2C_FILE_DESCRIPTOR = -1;

static char rds_output[9];

static void read_chip_registers(int fd)
{
    //Si4703 begins reading from upper register of 0x0A and reads to 0x0F, then loops to 0x00.
    uint8_t buffer[32];
    int rc = read(fd, buffer, 32);

    //TODO: handle rc
    pthread_mutex_lock(&register_mutex);
    for (int i = 0x0A, j = 0;; ++i, j += 2)
    {
        if (i == 0x10)
            i = 0;
        chip_regs[i] = buffer[j] << 8;
        chip_regs[i] |= buffer[j + 1];
        if (i == 0x09)
            break;
    }
    pthread_mutex_unlock(&register_mutex);
}

static void write_chip_registers(int fd)
{
    uint8_t write_buffer[12];
    // write sequence starts at address 0x02
    // only send the 0x02 - 0x07 control registers
    pthread_mutex_lock(&register_mutex);
    for (int reg = 0x02, i = 0; reg < 0x08; ++reg, i += 2)
    {
        write_buffer[i] = chip_regs[reg] >> 8;
        write_buffer[i + 1] = chip_regs[reg] & 0x00FF;
    }
    pthread_mutex_unlock(&register_mutex);

    int rc = write(fd, write_buffer, sizeof(write_buffer));
    // TODO: Check rc
}

static int read_channel(int *stereo, int *rssi)
{
    read_chip_registers(I2C_FILE_DESCRIPTOR);
    int channel = chip_regs[READCHAN] & 0x03FF; // Channel data is the lower 10 bits
    channel *= 2;
    channel += 875;
    *stereo = chip_regs[STATUSRSSI] & (1 << STEREO_BIT);
    *rssi = chip_regs[STATUSRSSI] & 0b01111111;
    return channel;
}



static void poll_rds_data()
{
    rds_data_t rds_data;
    rds_status_t rds_status;

    rds_status.radio_text = 0;
    rds_status.station_name = 0;

    // empty and null-terminate our string data
    memset(rds_data.program_service_name, ' ', sizeof(rds_data.program_service_name));
    memset(rds_data.radio_text, ' ', sizeof(rds_data.radio_text));
    rds_data.program_service_name[sizeof(rds_data.program_service_name) - 1] = 0;
    rds_data.radio_text[sizeof(rds_data.radio_text) - 1] = 0;

    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if (chip_regs[STATUSRSSI] & (1 << RDSR_BIT))
        {
            if ((((chip_regs[STATUSRSSI] & BLERA) >> 9) < 3) &&
                (((chip_regs[READCHAN] & BLERB) >> 14) < 3) &&
                (((chip_regs[READCHAN] & BLERC) >> 12) < 3) &&
                ((chip_regs[READCHAN] & BLERA) >> 10) < 3)
            {                

                uint16_t b = chip_regs[RDSB];
                uint16_t group = (b & GROUP_MASK) >> GROUP_OFFSET_BITS;
                int subgroup = (b & (1 << SUBGROUP_BIT)) >> SUBGROUP_BIT;
                char sg = subgroup == 0 ? 'A' : 'B';

                int traffic = (b & (1 << TRAFFIC_BIT)) >> TRAFFIC_BIT;
                uint16_t pty = (b & PTY_MASK) >> PTY_OFFSET;
                int music = (b & (1 << MUSIC_BIT)) >> MUSIC_BIT;
                int position = (b & POSITION_MASK);
                

                if (group == 0)
                {
                    int idx = (chip_regs[RDSB] & 0x03);

                    char ch = chip_regs[RDSD] >> 8;
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == ' ')
                    {
                        rds_data.program_service_name[idx * 2] = ch;
                    }
                    ch = chip_regs[RDSD] & 0xFF;
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == ' ')
                    {
                        rds_data.program_service_name[idx * 2 + 1] = ch;
                    }
                }

                pthread_mutex_lock(&rds_mutex);
                memcpy(rds_output, rds_data.program_service_name, sizeof(rds_data.program_service_name));
                //printf("\rProgram Service Name: %s", rds_output);
                pthread_mutex_unlock(&rds_mutex);
                
                
            }
            usleep(1000 * 50);
        }
        else
        {
            usleep(1000 * 30);
        }
    }
}

static int go_to_channel(int channel, int *stereo, int *rssi)
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

static int seek(int dir, int *stereo, int *rssi)
{
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
    while (1)
    {
        read_chip_registers(I2C_FILE_DESCRIPTOR);
        if ((chip_regs[STATUSRSSI] & (1 << STC_BIT)) != 0)
            break; // seek is done
        printf("Trying Station: %d\n", read_channel(stereo, rssi));        
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
    if (!rds_running){
        rds_running = true;
        int rc = pthread_create(&rds_thread_handle, NULL, poll_rds_data, NULL);
        pthread_detach(rds_thread_handle);
    }    
    pthread_mutex_lock(&rds_mutex);
    PyObject* result = PyUnicode_Decode(rds_output, sizeof(rds_output), "ascii", "ignore");
    pthread_mutex_unlock(&rds_mutex);
    return result;
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
    PyObject *result = PyTuple_New(3);
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
    PyObject *result = PyTuple_New(3);
    PyTuple_SetItem(result, 0, PyLong_FromLong(channel));
    PyTuple_SetItem(result, 1, PyBool_FromLong(stereo));
    PyTuple_SetItem(result, 2, PyLong_FromLong(rssi));
    return result;
}

static PyObject *method_getChannel(PyObject *self, PyObject *args)
{
    int stereo = 0;
    int rssi = 0;
    PyObject *result = PyTuple_New(3);
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
