#include "Arduino.h"
#include "Si4703_Breakout.h"
#include "Wire.h"

Si4703_Breakout::Si4703_Breakout(int resetPin, int sdioPin, int sclkPin)
{
    _resetPin = resetPin;
    _sdioPin = sdioPin;
    _sclkPin = sclkPin;
}

void Si4703_Breakout::powerOn()
{
    si4703_init();
}

void Si4703_Breakout::setChannel(int channel)
{
    //Freq(MHz) = 0.200(in USA) * Channel + 87.5MHz
    //97.3 = 0.2 * Chan + 87.5
    //9.8 / 0.2 = 49
    int newChannel = channel * 10; //973 * 10 = 9730
    newChannel -= 8750; //9730 - 8750 = 980
    newChannel /= 10; //980 / 10 = 98

    //These steps come from AN230 page 20 rev 0.5
    readRegisters();
    si4703_registers[CHANNEL] &= 0xFE00; //Clear out the channel bits
    si4703_registers[CHANNEL] |= newChannel; //Mask in the new channel
    si4703_registers[CHANNEL] |= (1<<TUNE); //Set the TUNE bit to start
    updateRegisters();

    //delay(60); //Wait 60ms - you can use or skip this delay

    //Poll to see if STC is set
    while(1) {
        readRegisters();
        if( (si4703_registers[STATUSRSSI] & (1<<STC)) != 0) break; //Tuning complete!
    }

    readRegisters();
    si4703_registers[CHANNEL] &= ~(1<<TUNE); //Clear the tune after a tune has completed
    updateRegisters();

    //Wait for the si4703 to clear the STC as well
    while(1) {
        readRegisters();
        if( (si4703_registers[STATUSRSSI] & (1<<STC)) == 0) break; //Tuning complete!
    }

    clearRdsInfo();
}

int Si4703_Breakout::seekUp()
{
    return seek(SEEK_UP);
}

int Si4703_Breakout::seekDown()
{
    return seek(SEEK_DOWN);
}

void Si4703_Breakout::setVolume(int volume)
{
    readRegisters(); //Read the current register set
    if(volume < 0) volume = 0;
    if (volume > 15) volume = 15;
    si4703_registers[SYSCONFIG2] &= 0xFFF0; //Clear volume bits
    si4703_registers[SYSCONFIG2] |= volume; //Set new volume
    updateRegisters(); //Update
}

bool Si4703_Breakout::toggleMute()
{
    readRegisters();
    si4703_registers[POWERCFG] ^= (1<<DMUTE); //Toggle Mute bit
    updateRegisters();

    return !(si4703_registers[POWERCFG] & (1<<DMUTE));
}

bool Si4703_Breakout::isStereo()
{
    readRegisters();

    return si4703_registers[STATUSRSSI] & (1<<STEREO);
}

uint8_t Si4703_Breakout::getSignalStrength()
{
    readRegisters();

    return si4703_registers[STATUSRSSI] & 0x00FF;
}

void Si4703_Breakout::readRDS()
{
    readRegisters();
    if(si4703_registers[STATUSRSSI] & (1<<RDSR)){
        RdsBlockA *blockA = (RdsBlockA*)&si4703_registers[RDSA];
        RdsBlockB *blockB = (RdsBlockB*)&si4703_registers[RDSB];

        rdsInfo.programIdentificationCode = blockA->programIdentificationCode;

        if (blockB->groupType == 2 && !blockB->isMessageVersionB) {
            /*
             * Radio Text
             */
            RdsBlockC_2_RadioText *blockC = (RdsBlockC_2_RadioText*)&si4703_registers[RDSC];
            RdsBlockD_2_RadioText *blockD = (RdsBlockD_2_RadioText*)&si4703_registers[RDSD];
            uint8_t tmp = blockB->extra;
            RdsBlockB_Extra_2_RadioText *blockBextra = (RdsBlockB_Extra_2_RadioText*)&tmp;

            if (blockBextra->clearScreen != radioTextLastStateClearBit) {
                radioTextLastStateClearBit = blockBextra->clearScreen;
                memset(tmpRadioText, 0, sizeof(tmpRadioText));
            }

            if (blockBextra->segmentOffset > 15) return;
            // todo: write in tmp var, not the same var the user get
            tmpRadioText[blockBextra->segmentOffset*4] = blockC->A;
            tmpRadioText[blockBextra->segmentOffset*4+1] = blockC->B;
            tmpRadioText[blockBextra->segmentOffset*4+2] = blockD->C;
            tmpRadioText[blockBextra->segmentOffset*4+3] = blockD->D;

            uint8_t copyFullMessage = 1;
            uint8_t firstSpacePosWithoutCharsAfter = 0;
            for (int i=0; i<64; i++) {
                // Each message must ends with the carriage return (0D hex) character if the length is < 64
                if (tmpRadioText[i] == '\r') {
                    // \r found: copy all chars before \r to radioText and fill the end with zeros
                    memset(rdsInfo.radioText, 0, sizeof(rdsInfo.radioText));
                    memcpy(rdsInfo.radioText, tmpRadioText, i);
                    copyFullMessage = 0;
                    break;
                }
                if (tmpRadioText[i] == 0) {
                    // one char is \0 but we didn't see any \r : message is incomplete
                    copyFullMessage = 0;
                    break;
                }

                // Some stations doesn't use '\r' and send spaces 0x20 after the text to fill 64 chars. We trim it.
                if (tmpRadioText[i] == ' ' && !firstSpacePosWithoutCharsAfter) {
                    firstSpacePosWithoutCharsAfter = i; // Current char is a space and previous char is not space. We update the var to keep the position
                } else if (tmpRadioText[i] != ' ') {
                    firstSpacePosWithoutCharsAfter = 0; // We found a char after one (or more) space, reset the position var
                }
            }

            if (copyFullMessage) {
                // If firstSpacePosWithoutCharsAfter is not zero, we trim the string. Otherwise we copy the full message
                memcpy(rdsInfo.radioText, tmpRadioText, firstSpacePosWithoutCharsAfter ? firstSpacePosWithoutCharsAfter : sizeof(rdsInfo.radioText));
            }
        } else if (blockB->groupType == 0) {
            /*
             * Station Name
             * Alternative Frequency
             */
            RdsBlockD_0_StationName *blockD = (RdsBlockD_0_StationName*)&si4703_registers[RDSD];
            uint8_t tmp = blockB->extra;
            RdsBlockB_Extra_0_StationName *blockBextra = (RdsBlockB_Extra_0_StationName*)&tmp;

            if (!blockB->isMessageVersionB) {
                RdsBlockC_0_AlternativeFrequency *blockC = (RdsBlockC_0_AlternativeFrequency*)&si4703_registers[RDSC];

                if (blockC->AF0 > 224 && blockC->AF0 < 250) {
                    rdsInfo.alternateFrequenciesCount = blockC->AF0 - 224;
                    alternateFrequenciesIndex = 0;
                }
                if (blockC->AF1 > 224 && blockC->AF1 < 250) {
                    rdsInfo.alternateFrequenciesCount = blockC->AF1 - 224;
                    alternateFrequenciesIndex = 0;
                }

                if (blockC->AF0 > 0 && blockC->AF0 < 205 && alternateFrequenciesIndex < rdsInfo.alternateFrequenciesCount) {
                    rdsInfo.alternateFrequencies[alternateFrequenciesIndex++] = 875 + blockC->AF0;
                }
                if (blockC->AF1 > 0 && blockC->AF1 < 205 && alternateFrequenciesIndex < rdsInfo.alternateFrequenciesCount) {
                    rdsInfo.alternateFrequencies[alternateFrequenciesIndex++] = 875 + blockC->AF1;
                }
            }

            if (blockBextra->segmentOffset > 3) return;

            tmpStationName[blockBextra->segmentOffset*2] = blockD->A;
            tmpStationName[blockBextra->segmentOffset*2+1] = blockD->B;
            uint8_t copyFullMessage = 1;
            for (int i=0; i<8; i++) {
                if (tmpStationName[i] == 0) {
                    // one char is \0 : message is incomplete
                    copyFullMessage = 0;
                    break;
                }
            }

            if (copyFullMessage) {
                memcpy(rdsInfo.stationName, tmpStationName, sizeof(rdsInfo.stationName));
            }
        }
        delay(40); //Wait for the RDS bit to clear
    }
    else {
        delay(30); //From AN230, using the polling method 40ms should be sufficient amount of time between checks
    }
}




//To get the Si4703 inito 2-wire mode, SEN needs to be high and SDIO needs to be low after a reset
//The breakout board has SEN pulled high, but also has SDIO pulled high. Therefore, after a normal power up
//The Si4703 will be in an unknown state. RST must be controlled
void Si4703_Breakout::si4703_init() 
{
    pinMode(_resetPin, OUTPUT);
    pinMode(_sdioPin, OUTPUT); //SDIO is connected to A4 for I2C
    digitalWrite(_sdioPin, LOW); //A low SDIO indicates a 2-wire interface
    digitalWrite(_resetPin, LOW); //Put Si4703 into reset
    delay(1); //Some delays while we allow pins to settle
    digitalWrite(_resetPin, HIGH); //Bring Si4703 out of reset with SDIO set to low and SEN pulled high with on-board resistor
    delay(1); //Allow Si4703 to come out of reset

    Wire.begin(); //Now that the unit is reset and I2C inteface mode, we need to begin I2C

    readRegisters(); //Read the current register set
    //si4703_registers[0x07] = 0xBC04; //Enable the oscillator, from AN230 page 9, rev 0.5 (DOES NOT WORK, wtf Silicon Labs datasheet?)
    si4703_registers[0x07] = 0x8100; //Enable the oscillator, from AN230 page 9, rev 0.61 (works)
    updateRegisters(); //Update

    delay(500); //Wait for clock to settle - from AN230 page 9

    readRegisters(); //Read the current register set
    si4703_registers[POWERCFG] = 0x4001; //Enable the IC
    //  si4703_registers[POWERCFG] |= (1<<SMUTE) | (1<<DMUTE); //Disable Mute, disable softmute
    si4703_registers[SYSCONFIG1] |= (1<<RDS); //Enable RDS

    si4703_registers[SYSCONFIG1] |= (1<<DE); //50kHz Europe setup
    si4703_registers[SYSCONFIG2] |= (1<<SPACE0); //100kHz channel spacing for Europe

    si4703_registers[SYSCONFIG2] &= 0xFFF0; //Clear volume bits
    si4703_registers[SYSCONFIG2] |= 0x0001; //Set volume to lowest
    updateRegisters(); //Update

    delay(110); //Max powerup time, from datasheet page 13
}

//Read the entire register control set from 0x00 to 0x0F
void Si4703_Breakout::readRegisters(){

    //Si4703 begins reading from register upper register of 0x0A and reads to 0x0F, then loops to 0x00.
    Wire.requestFrom(SI4703, 32); //We want to read the entire register set from 0x0A to 0x09 = 32 bytes.

    while(Wire.available() < 32) ; //Wait for 16 words/32 bytes to come back from slave I2C device
    //We may want some time-out error here

    //Remember, register 0x0A comes in first so we have to shuffle the array around a bit
    for(int x = 0x0A ; ; x++) { //Read in these 32 bytes
        if(x == 0x10) x = 0; //Loop back to zero
        si4703_registers[x] = Wire.read() << 8;
        si4703_registers[x] |= Wire.read();
        if(x == 0x09) break; //We're done!
    }
}

//Write the current 9 control registers (0x02 to 0x07) to the Si4703
//It's a little weird, you don't write an I2C addres
//The Si4703 assumes you are writing to 0x02 first, then increments
byte Si4703_Breakout::updateRegisters() {

    Wire.beginTransmission(SI4703);
    //A write command automatically begins with register 0x02 so no need to send a write-to address
    //First we send the 0x02 to 0x07 control registers
    //In general, we should not write to registers 0x08 and 0x09
    for(int regSpot = 0x02 ; regSpot < 0x08 ; regSpot++) {
        byte high_byte = si4703_registers[regSpot] >> 8;
        byte low_byte = si4703_registers[regSpot] & 0x00FF;

        Wire.write(high_byte); //Upper 8 bits
        Wire.write(low_byte); //Lower 8 bits
    }

    //End this transmission
    byte ack = Wire.endTransmission();
    if(ack != 0) { //We have a problem!
        return(FAIL);
    }

    return(SUCCESS);
}

//Seeks out the next available station
//Returns the freq if it made it
//Returns zero if failed
int Si4703_Breakout::seek(byte seekDirection){
    readRegisters();
    //Set seek mode wrap bit
    si4703_registers[POWERCFG] |= (1<<SKMODE); //Allow wrap
    //si4703_registers[POWERCFG] &= ~(1<<SKMODE); //Disallow wrap - if you disallow wrap, you may want to tune to 87.5 first
    if(seekDirection == SEEK_DOWN) si4703_registers[POWERCFG] &= ~(1<<SEEKUP); //Seek down is the default upon reset
    else si4703_registers[POWERCFG] |= 1<<SEEKUP; //Set the bit to seek up

    si4703_registers[POWERCFG] |= (1<<SEEK); //Start seek
    updateRegisters(); //Seeking will now start

    //Poll to see if STC is set
    while(1) {
        readRegisters();
        if((si4703_registers[STATUSRSSI] & (1<<STC)) != 0) break; //Tuning complete!
    }

    readRegisters();
    int valueSFBL = si4703_registers[STATUSRSSI] & (1<<SFBL); //Store the value of SFBL
    si4703_registers[POWERCFG] &= ~(1<<SEEK); //Clear the seek bit after seek has completed
    updateRegisters();

    //Wait for the si4703 to clear the STC as well
    while(1) {
        readRegisters();
        if( (si4703_registers[STATUSRSSI] & (1<<STC)) == 0) break; //Tuning complete!
    }

    clearRdsInfo();

    if(valueSFBL) { //The bit was set indicating we hit a band limit or failed to find a station
        return(0);
    }
    return getChannel();
}

//Reads the current channel from READCHAN
//Returns a number like 973 for 97.3MHz
int Si4703_Breakout::getChannel() {
    readRegisters();
    int channel = si4703_registers[READCHAN] & 0x03FF; //Mask out everything but the lower 10 bits
    //Freq(MHz) = 0.100(in Europe) * Channel + 87.5MHz
    //X = 0.1 * Chan + 87.5
    channel += 875; //98 + 875 = 973
    return(channel);
}

void Si4703_Breakout::clearRdsInfo()
{
    rdsInfo = RdsInfo();

    memset(tmpStationName, 0, sizeof(tmpStationName));
    memset(tmpRadioText, 0, sizeof(tmpRadioText));

    // put incorrect value to force clear
    radioTextLastStateClearBit = 0xFF;

    alternateFrequenciesIndex = 0;
}
