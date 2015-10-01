// X6_1000.cpp
//
// Provides interface to BBN's custom firmware for the II X6-1000 card
//
// Original authors: Brian Donnovan, Colm Ryan and Blake Johnson
//
// Copyright 2013-2015 Raytheon BBN Technologies

#include <algorithm>  // std::max
#include <chrono>     // std::chrono::seconds etc.
#include <thread>     // std::this_thread

#include "X6_1000.h"
#include "X6_errno.h"
#include "helpers.h"
#include "constants.h"

#include <IppMemoryUtils_Mb.h>  // for Init::UsePerformanceMemoryFunctions
#include <BufferDatagrams_Mb.h> // for ShortDG

using namespace Innovative;

// constructor
X6_1000::X6_1000() :
    isOpen_{false},
    isRunning_{false},
    needToInit_{true},
    activeInputChannels_{true, true},
    activeOutputChannels_{false, false, false, false},
    refSource_{INTERNAL_REFERENCE}
    {

    timer_.Interval(1000);

    // Use IPP performance memory functions.
    Init::UsePerformanceMemoryFunctions();
}

X6_1000::~X6_1000() {
	if (isOpen_) close();
}

void X6_1000::open(int deviceID) {
    /* Connects to the II module with the given device ID returns MODULE_ERROR
     * if the device cannot be found
     */

    if (isOpen_) return;

    deviceID_ = deviceID;

    //  Configure Trigger Manager Event Handlers
    trigger_.OnDisableTrigger.SetEvent(this, &X6_1000::HandleDisableTrigger);
    trigger_.OnExternalTrigger.SetEvent(this, &X6_1000::HandleExternalTrigger);
    trigger_.OnSoftwareTrigger.SetEvent(this, &X6_1000::HandleSoftwareTrigger);
    trigger_.DelayedTrigger(true); // trigger delayed after start

    //  Configure Module Event Handlers
    module_.OnBeforeStreamStart.SetEvent(this, &X6_1000::HandleBeforeStreamStart);
    module_.OnBeforeStreamStart.Synchronize();
    module_.OnAfterStreamStart.SetEvent(this, &X6_1000::HandleAfterStreamStart);
    module_.OnAfterStreamStart.Synchronize();
    module_.OnAfterStreamStop.SetEvent(this, &X6_1000::HandleAfterStreamStop);
    module_.OnAfterStreamStop.Synchronize();

    // Stream Event Handlers
    stream_.DirectDataMode(false);
    stream_.OnVeloDataAvailable.SetEvent(this, &X6_1000::HandleDataAvailable);

    stream_.RxLoadBalancing(false);
    stream_.TxLoadBalancing(false);

    timer_.OnElapsed.SetEvent(this, &X6_1000::HandleTimer);
    timer_.OnElapsed.Thunk();

    // Insure BM size is a multiple of four MB
    const int RxBmSize = std::max(BusmasterSize/4, 1) * 4;
    const int TxBmSize = std::max(BusmasterSize/4, 1) * 4;
    module_.IncomingBusMasterSize(RxBmSize * Meg);
    module_.OutgoingBusMasterSize(TxBmSize * Meg);

    module_.Target(deviceID);

    try {
        module_.Open();
        FILE_LOG(logINFO) << "Opened Device " << deviceID;
        FILE_LOG(logINFO) << "Bus master size: Input => " << RxBmSize << " MB" << " Output => " << TxBmSize << " MB";
    }
    catch(...) {
        FILE_LOG(logINFO) << "Module Device Open Failure!";
        throw X6_MODULE_ERROR;
    }

    module_.Reset();
    FILE_LOG(logINFO) << "X6 module opened and reset successfully...";

    needToInit_ = true;

    isOpen_ = true;

    log_card_info();

    //  Connect Stream
    stream_.ConnectTo(&module_);
    FILE_LOG(logINFO) << "Stream Connected...";

    prefillPacketCount_ = stream_.PrefillPacketCount();
    FILE_LOG(logDEBUG) << "Stream prefill packet count: " << prefillPacketCount_;

    //Set some default clocking so get_pll_frequency and *_nco_frequency work
    //Use internal reference and 1GS ADC/DAC
    FILE_LOG(logDEBUG) << "Setting default clocking to internal 10MHz reference.";
    module_.Clock().Reference(IX6ClockIo::rsInternal);
    module_.Clock().ReferenceFrequency(10e6);
    module_.Clock().Source(IX6ClockIo::csInternal);
    module_.Clock().Frequency(1e9);
  }

void X6_1000::init() {
    //Initialize/preconfigure the stream for the current channel configuration

    set_active_channels();

    //For now hard-code in some clocking and routing
    module_.Clock().ExternalClkSelect(IX6ClockIo::cslFrontPanel);
    module_.Clock().Source(IX6ClockIo::csInternal);

    //Switch between internal and external reference
    module_.Clock().ReferenceFrequency(10e6); //assume 10MHz for now
    module_.Clock().Reference((refSource_ == EXTERNAL_REFERENCE) ? IX6ClockIo::rsExternal : IX6ClockIo::rsInternal);

    //Run the ADC and DAC at full rate for now
    module_.Clock().Adc().Frequency(1000 * 1e6);
    module_.Clock().Dac().Frequency(1000 * 1e6);
    // Readback Frequency
    double adc_freq_actual = module_.Clock().Adc().FrequencyActual();
    double dac_freq_actual = module_.Clock().Dac().FrequencyActual();
    double adc_freq = module_.Clock().Adc().Frequency();
    double dac_freq = module_.Clock().Dac().Frequency();

    FILE_LOG(logDEBUG) << "Desired PLL Frequencies: [ADC] " << adc_freq << " [DAC] " << dac_freq;
    FILE_LOG(logDEBUG) << "Actual PLL Frequencies: [ADC] " << adc_freq_actual << " [DAC] " << dac_freq_actual;

    FILE_LOG(logDEBUG) << "AFE reg. 0x98 (DAC calibration): " << hexn<8> << read_wishbone_register(0x0800, 0x98);
    FILE_LOG(logDEBUG) << "Preconfiguring stream...";
    stream_.Preconfigure();
    FILE_LOG(logDEBUG) << "AFE reg. 0x98 (DAC calibration): " << hexn<8> << read_wishbone_register(0x0800, 0x98);

    needToInit_ = false;
}

void X6_1000::close() {
    stream_.Disconnect();
    module_.Close();

    isOpen_ = false;
    FILE_LOG(logINFO) << "Closed connection to device " << deviceID_;
}

uint16_t X6_1000::get_firmware_version(MODULE_FIRMWARE_VERSION mod) {
    uint32_t regVal;
    switch (mod) {
        case BBN_PG:
            //Read from PG regs
            regVal = read_wishbone_register(BASE_PG[0], WB_PG_MODULE_FIRMWARE_VERSION);
            break;

        case BBN_X6:
        case BBN_QDSP:
            regVal = read_dsp_register(0, WB_QDSP_MODULE_FIRMWARE_VERSION);
            break;
    }

    uint16_t ver;
    switch (mod) {
        case BBN_X6:
            ver =  static_cast<uint16_t>(regVal >> 16);
            break;
        case BBN_PG:
        case BBN_QDSP:
            ver = static_cast<uint16_t>(regVal & 0x0000ffff);
        default:
            ver = 0;
    }
    return ver;
}

float X6_1000::get_logic_temperature() {
    return static_cast<float>(module_.Thermal().LogicTemperature());
}

void X6_1000::set_reference_source(REFERENCE_SOURCE ref) {
    if ( refSource_ != ref ) {
        refSource_ = ref;
        needToInit_ = true;
    }
}

REFERENCE_SOURCE X6_1000::get_reference_source() {
    return refSource_;
}

double X6_1000::get_pll_frequency() {
    double freq = module_.Clock().FrequencyActual();
    FILE_LOG(logINFO) << "PLL frequency for X6: " << freq;
    return freq;
}

void X6_1000::set_trigger_source(TRIGGER_SOURCE trgSrc) {
    // cache trigger source
    triggerSource_ = trgSrc;
}

TRIGGER_SOURCE X6_1000::get_trigger_source() const {
    return triggerSource_;
}

void X6_1000::set_trigger_delay(float delay) {
    // going to require a trigger engine modification to work
    // leaving as a TODO for now
    // Something like this might work:
    // trigger_.DelayedTriggerPeriod(delay);
}

void X6_1000::set_decimation(bool enabled, int factor) {
    module_.Input().Decimation((enabled ) ? factor : 0);
}

int X6_1000::get_decimation() {
    int decimation = module_.Input().Decimation();
    return (decimation > 0) ? decimation : 1;
}

void X6_1000::set_frame(int recordLength) {
    FILE_LOG(logINFO) << "Setting recordLength_ = " << recordLength;

    recordLength_ = recordLength;

    // setup the trigger window size
    int frameGranularity = module_.Input().Info().TriggerFrameGranularity();
    if (recordLength % frameGranularity != 0) {
        FILE_LOG(logERROR) << "Invalid frame size: " << recordLength;
        throw X6_INVALID_FRAMESIZE;
    }
    module_.Input().Trigger().FramedMode(true);
    module_.Input().Trigger().Edge(true);
    module_.Input().Trigger().FrameSize(recordLength);

    // (some of?) the following seems to be necessary to get external triggering to work
    module_.Input().Pulse().Reset();
    module_.Input().Pulse().Enabled(false);

    module_.Output().Pulse().Reset();
    module_.Output().Pulse().Enabled(false);

    //Set the QDSP register
    for (int inst = 0; inst <= 1; ++inst) {
        write_dsp_register(inst, WB_QDSP_RECORD_LENGTH, recordLength_);
    }
}

void X6_1000::set_averager_settings(const int & recordLength, const int & numSegments, const int & waveforms,  const int & roundRobins) {
    set_frame(recordLength);
    numSegments_ = numSegments;
    waveforms_ = waveforms;
    roundRobins_ = roundRobins;
    numRecords_ = numSegments * waveforms * roundRobins;
}

void X6_1000::enable_stream(unsigned a, unsigned b, unsigned c) {
    FILE_LOG(logINFO) << "Enable stream " << a << "." << b << "." << c;

    // set the appropriate bit in stream_enable register
    int reg = read_dsp_register(a-1, WB_QDSP_STREAM_ENABLE);
    int bit = (b==0) ? c : 15 + b + (c & 0x1)*4;
    reg |= 1 << bit;
    FILE_LOG(logDEBUG4) << "Setting stream_enable register bit " << bit << " by writing register value " << hexn<8> << reg;
    write_dsp_register(a-1, WB_QDSP_STREAM_ENABLE, reg);

    QDSPStream stream = QDSPStream(a, b, c);
    FILE_LOG(logDEBUG2) << "Assigned stream " << a << "." << b << "." << c << " to streamID " << hexn<4> << stream.streamID;
    activeQDSPStreams_[stream.streamID] = stream;
}

void X6_1000::disable_stream(unsigned a, unsigned b, unsigned c) {
    // clear the appropriate bit in stream_enable register
    int reg = read_dsp_register(a-1, WB_QDSP_STREAM_ENABLE);
    int bit = (b==0) ? c : 15 + b + (c & 0x1)*4;
    reg &= ~(1 << bit);
    FILE_LOG(logDEBUG4) << "Clearing stream_enable register bit " << bit << " by writing register value " << hexn<8> << reg;
    write_dsp_register(a-1, WB_QDSP_STREAM_ENABLE, reg);

    //Find the channel
    uint16_t streamID = QDSPStream(a,b,c).streamID;
    if (activeQDSPStreams_.count(streamID)) {
        activeQDSPStreams_.erase(streamID);
        FILE_LOG(logINFO) << "Disabling stream " << a << "." << b << "." << c;
    }
    else {
        FILE_LOG(logERROR) << "Tried to disable stream " << a << "." << b << "." << c << " which was not enabled.";
    }
}

void X6_1000::set_input_channel_enable(unsigned channel, bool enable) {
    if (activeInputChannels_[channel] != enable) {
        activeInputChannels_[channel] = enable;
        needToInit_ = true;
    }
}

bool X6_1000::get_input_channel_enable(unsigned channel) {
    return activeInputChannels_[channel];
}

void X6_1000::set_output_channel_enable(unsigned channel, bool enable) {
    if (activeOutputChannels_[channel] != enable) {
        activeOutputChannels_[channel] = enable;
        needToInit_ = true;
    }
}

bool X6_1000::get_output_channel_enable(unsigned channel) {
    return activeOutputChannels_[channel];
}

void X6_1000::set_nco_frequency(int a, int b, double freq) {
    // NCO runs at quarter rate
    double nfreq = 4 * freq/get_pll_frequency();
    int32_t phase_increment = rint(nfreq * (1 << 24)); //24 bit precision on DDS
    FILE_LOG(logDEBUG3) << "Setting channel " << a << "." << b << " NCO frequency to: " << freq/1e6 << " MHz (" << phase_increment << ")";
    write_dsp_register(a-1, WB_QDSP_PHASE_INC + (b-1), phase_increment);
}

double X6_1000::get_nco_frequency(int a, int b) {
    uint32_t phaseInc = read_dsp_register(a-1, WB_QDSP_PHASE_INC + (b-1));
    //Undo the math in set_nco_frequency
    return static_cast<double>(phaseInc) / (1 << 24) * get_pll_frequency() / 4;
}

void X6_1000::set_threshold(int a, int c, double threshold) {
    // Results are sfix32_15, so scale threshold by 2^15.
    int32_t scaled_threshold = threshold * (1 << 15);
    FILE_LOG(logDEBUG3) << "Setting channel " << a << ".0." << c << " threshold to: " << threshold << " (" << scaled_threshold << ")";
    write_dsp_register(a-1, WB_QDSP_THRESHOLD + (c-1), scaled_threshold);
}

double X6_1000::get_threshold(int a, int c) {
    int32_t fixedThreshold = read_dsp_register(a-1, WB_QDSP_THRESHOLD + (c-1));
    //Undo the scaling above
    return static_cast<double>(fixedThreshold) / (1 << 15);
}

void X6_1000::write_kernel(int a, int b, int c, const vector<complex<double>> & kernel) {
    //TODO throw error if kernel too long
    //TODO throw error if a,b,c are not a kernel integration channel
    FILE_LOG(logDEBUG3) << "Writing channel " << a << "." << b << "." << c << " kernel of length to: " << kernel.size();

    //Depending on raw or demod integrator we are enumerated by c or b
    int KI = (b==0) ? c : b;
    uint32_t wbLengthReg = (b==0) ?  WB_QDSP_RAW_KERNEL_LENGTH : WB_QDSP_DEMOD_KERNEL_LENGTH;
    uint32_t wbAddrDataReg = (b==0) ?  WB_QDSP_RAW_KERNEL_ADDR_DATA : WB_QDSP_DEMOD_KERNEL_ADDR_DATA;

    //Write the length register
    write_dsp_register(a-1, wbLengthReg + (KI-1), kernel.size());

    //Kernel memory as address/data pairs
    for (size_t ct = 0; ct < kernel.size(); ct++) {
        int32_t scaled_re = std::real(kernel[ct]) * ((1 << 15) - 1);
        int32_t scaled_im = std::imag(kernel[ct]) * ((1 << 15) - 1);
        uint32_t packedval = (scaled_im << 16) | (scaled_re & 0xffff);
        write_dsp_register(a-1, wbAddrDataReg + 2*(KI-1), ct);
        write_dsp_register(a-1, wbAddrDataReg + 2*(KI-1) + 1, packedval);
    }
}

complex<double> X6_1000::read_kernel(unsigned a, unsigned b, unsigned c, unsigned addr) {
  //Read kernel memory at the specified address

  //Depending on raw or demod integrator we are enumerated by c or b
  int KI = (b==0) ? c : b;
  uint32_t wbAddrDataReg = (b==0) ?  WB_QDSP_RAW_KERNEL_ADDR_DATA : WB_QDSP_DEMOD_KERNEL_ADDR_DATA;

  //Write the address register
  write_dsp_register(a-1, wbAddrDataReg + 2*(KI-1), addr);

  //Read the data
  uint32_t packedVal = read_dsp_register(a-1, wbAddrDataReg + 2*(KI-1) + 1);

  //Scale and convert back to complex
  //The conversion from unsigned to signed is not guaranteed to keep the bit pattern
  //However for gcc using two's complement it does
  //See http://stacko;verflow.com/a/4219558 and http://stackoverflow.com/q/13150449
  int16_t fixedReal = packedVal & 0xffff;
  int16_t fixedImag = packedVal >> 16;
  return complex<double>(static_cast<double>(fixedReal) / ((1 << 15) - 1),
                        static_cast<double>(fixedImag) / ((1 << 15) - 1) );
}

void X6_1000::set_active_channels() {
    module_.Output().ChannelDisableAll();
    module_.Input().ChannelDisableAll();

    for (unsigned ct = 0; ct < activeInputChannels_.size(); ct++) {
        FILE_LOG(logINFO) << "Physical input channel " << ct << (activeInputChannels_[ct] ? " enabled" : " disabled");
        module_.Input().ChannelEnabled(ct, activeInputChannels_[ct]);
    }

    for (unsigned ct = 0; ct < activeOutputChannels_.size(); ct++) {
        FILE_LOG(logINFO) << "Physical output channel " << ct << (activeOutputChannels_[ct] ? " enabled" : " disabled");
        module_.Output().ChannelEnabled(ct, activeOutputChannels_[ct]);
    }
}

void X6_1000::log_card_info() {

    FILE_LOG(logINFO) << std::hex << "Logic Version: " << module_.Info().FpgaLogicVersion()
        << ", Hdw Variant: " << module_.Info().FpgaHardwareVariant()
        << ", Revision: " << module_.Info().PciLogicRevision()
        << ", Subrevision: " << module_.Info().FpgaLogicSubrevision();

    FILE_LOG(logINFO)  << std::hex << "Board Family: " << module_.Info().PciLogicFamily()
        << ", Type: " << module_.Info().PciLogicType()
        << ", Board Revision: " << module_.Info().PciLogicPcb()
        << ", Chip: " << module_.Info().FpgaChipType();

    FILE_LOG(logINFO)  << "PCI Express Lanes: " << module_.Debug()->LaneCount();
}

void X6_1000::acquire() {
    //Configure the streams (calibrate DACs) if necessary
    if (needToInit_) {
        init();
    }

    //Some error checking frame sizes
    //Because of some FIFO's and clocking  we can't have more than 4096 samples
    QDSPStream rawStream1 = QDSPStream(1, 0, 0);
    QDSPStream rawStream2 = QDSPStream(2, 0, 0);

    if (activeQDSPStreams_.count(rawStream1.streamID) || activeQDSPStreams_.count(rawStream2.streamID)){
        if (recordLength_ > MAX_LENGTH_RAW_STREAM) {
            throw X6_RAW_STREAM_TOO_LONG;
        }
    }

    trigger_.DelayedTriggerPeriod(0);
    trigger_.ExternalTrigger(triggerSource_ == EXTERNAL_TRIGGER ? true : false);
    trigger_.AtConfigure();

    module_.Output().Trigger().FramedMode(true);
    module_.Output().Trigger().Edge(true);
    module_.Output().Trigger().FrameSize(1024);

    module_.Input().Trigger().FramedMode(true);
    module_.Input().Trigger().Edge(true);
    module_.Input().Trigger().FrameSize(1024);

    //  Route External Trigger source
    module_.Output().Trigger().ExternalSyncSource( IX6IoDevice::essFrontPanel );
    module_.Input().Trigger().ExternalSyncSource( IX6IoDevice::essFrontPanel );

    // Initialize VeloMergeParsers with stream IDs
    VMPs_.clear();
    VMPs_.resize(3);

    physChans_.clear();
    virtChans_.clear();
    resultChans_.clear();

    for (auto kv : activeQDSPStreams_){
        switch (kv.second.type) {
            case PHYSICAL:
                physChans_.push_back(kv.first);
                FILE_LOG(logDEBUG) << "ADC physical stream ID: " << hexn<4> << kv.first;
                break;
            case DEMOD:
                virtChans_.push_back(kv.first);
                FILE_LOG(logDEBUG) << "ADC virtual stream ID: " << hexn<4> << kv.first;
                break;
            case RESULT:
                resultChans_.push_back(kv.first);
                FILE_LOG(logDEBUG) << "ADC result stream ID: " << hexn<4> << kv.first;
        }
    }
    initialize_accumulators();
    initialize_correlators();

    VMPs_[0].Init(physChans_);
    VMPs_[0].OnDataAvailable.SetEvent(this, &X6_1000::HandlePhysicalStream);

    VMPs_[1].Init(virtChans_);
    VMPs_[1].OnDataAvailable.SetEvent(this, &X6_1000::HandleVirtualStream);

    VMPs_[2].Init(resultChans_);
    VMPs_[2].OnDataAvailable.SetEvent(this, &X6_1000::HandleResultStream);

    //Now set the buffers sizes to fire when a full record length is in
    int samplesPerWord = module_.Input().Info().SamplesPerWord();
    FILE_LOG(logDEBUG) << "samplesPerWord = " << samplesPerWord;
    // calculate packet size for physical and virtual channels
    int packetSize = recordLength_/samplesPerWord/get_decimation()/RAW_DECIMATION_FACTOR;
    FILE_LOG(logDEBUG) << "Physical channel packetSize = " << packetSize;
    VMPs_[0].Resize(packetSize);
    VMPs_[0].Clear();

    //Vitual channels are complex so they get a factor of two.
    packetSize = 2*recordLength_/samplesPerWord/get_decimation()/DEMOD_DECIMATION_FACTOR;
    FILE_LOG(logDEBUG) << "Virtual channel packetSize = " << packetSize;
    VMPs_[1].Resize(packetSize);
    VMPs_[1].Clear();

    //Result channels are complex 32bit integers
    packetSize = 2;
    FILE_LOG(logDEBUG) << "Result channel packetSize = " << packetSize;
    VMPs_[2].Resize(packetSize);
    VMPs_[2].Clear();

    recordsTaken_ = 0;

    module_.Velo().LoadAll_VeloDataSize(0x4000);
    module_.Velo().ForceVeloPacketSize(false);

    // is this necessary??
    stream_.PrefillPacketCount(prefillPacketCount_);

    trigger_.AtStreamStart();

    FILE_LOG(logDEBUG) << "AFE reg. 0x5 (adc/dac run): " << hexn<8> << read_wishbone_register(0x0800, 0x5);
    FILE_LOG(logDEBUG) << "AFE reg. 0x8 (adc en): " << hexn<8> << read_wishbone_register(0x0800, 0x8);
    FILE_LOG(logDEBUG) << "AFE reg. 0x9 (adc trigger): " << hexn<8> << read_wishbone_register(0x0800, 0x9);
    FILE_LOG(logDEBUG) << "AFE reg. 0x80 (dac en): " << hexn<8> << read_wishbone_register(0x0800, 0x80);
    FILE_LOG(logDEBUG) << "AFE reg. 0x81 (dac trigger): " << hexn<8> << read_wishbone_register(0x0800, 0x81);

    // flag must be set before calling stream start
    isRunning_ = true;

    //  Start Streaming
    FILE_LOG(logINFO) << "Arming acquisition";
    stream_.Start();

    FILE_LOG(logDEBUG) << "AFE reg. 0x5 (adc/dac run): " << hexn<8> << read_wishbone_register(0x0800, 0x5);
    FILE_LOG(logDEBUG) << "AFE reg. 0x8 (adc en): " << hexn<8> << read_wishbone_register(0x0800, 0x8);
    FILE_LOG(logDEBUG) << "AFE reg. 0x9 (adc trigger): " << hexn<8> << read_wishbone_register(0x0800, 0x9);
    FILE_LOG(logDEBUG) << "AFE reg. 0x80 (dac en): " << hexn<8> << read_wishbone_register(0x0800, 0x80);
    FILE_LOG(logDEBUG) << "AFE reg. 0x81 (dac trigger): " << hexn<8> << read_wishbone_register(0x0800, 0x81);
}

void X6_1000::wait_for_acquisition(unsigned timeOut){
    /* Blocking wait until all the records have been acquired. */

    auto start = std::chrono::system_clock::now();
    auto end = start + std::chrono::seconds(timeOut);
    while (get_is_running()) {
        if (std::chrono::system_clock::now() > end)
            throw X6_TIMEOUT;
        std::this_thread::sleep_for( std::chrono::milliseconds(100) );
    }
}

void X6_1000::stop() {
    isRunning_ = false;
    stream_.Stop();
    timer_.Enabled(false);
    trigger_.AtStreamStop();
}

bool X6_1000::get_is_running() {
    return isRunning_;
}

bool X6_1000::get_has_new_data() {
    // determines if new data has arrived since the last call
    size_t currentRecords = 0;
    for (auto & kv : accumulators_) {
        currentRecords = max(currentRecords, kv.second.recordsTaken);
    }

    bool result = (currentRecords > recordsTaken_);
    recordsTaken_ = currentRecords;
    return result;
}

void X6_1000::transfer_waveform(QDSPStream stream, double * buffer, size_t length) {
    //Check we have the stream
    uint16_t sid = stream.streamID;
    if(activeQDSPStreams_.find(sid) == activeQDSPStreams_.end()){
        FILE_LOG(logERROR) << "Tried to transfer waveform from disabled stream.";
        throw X6_INVALID_CHANNEL;
    }
    //Don't copy more than we have
    if (length < accumulators_[sid].get_buffer_size() ) {
        FILE_LOG(logERROR) << "Not enough memory allocated in buffer to transfer waveform.";
    }
    accumulators_[sid].snapshot(buffer);
}

void X6_1000::transfer_variance(QDSPStream stream, double * buffer, size_t length) {
    //Check we have the stream
    uint16_t sid = stream.streamID;
    if(activeQDSPStreams_.find(sid) == activeQDSPStreams_.end()){
        FILE_LOG(logERROR) << "Tried to transfer waveform variance from disabled stream.";
        throw X6_INVALID_CHANNEL;
    }
    //Don't copy more than we have
    if (length < accumulators_[sid].get_buffer_size() ) {
        FILE_LOG(logERROR) << "Not enough memory allocated in buffer to transfer variance.";
    }
    accumulators_[sid].snapshot_variance(buffer);
}

void X6_1000::transfer_correlation(vector<QDSPStream> & streams, double *buffer, size_t length) {
    // check that we have the correlator
    vector<uint16_t> sids(streams.size());
    for (size_t i = 0; i < streams.size(); i++)
        sids[i] = streams[i].streamID;
    if (correlators_.find(sids) == correlators_.end()) {
        FILE_LOG(logERROR) << "Tried to transfer invalid correlator.";
        throw X6_INVALID_CHANNEL;
    }
    // Don't copy more than we have
    if (length < correlators_[sids].get_buffer_size()) {
        FILE_LOG(logERROR) << "Not enough memory allocated in buffer to transfer correlator.";
    }
    correlators_[sids].snapshot(buffer);
}

void X6_1000::transfer_correlation_variance(vector<QDSPStream> & streams, double *buffer, size_t length) {
    // check that we have the correlator
    vector<uint16_t> sids(streams.size());
    for (size_t i = 0; i < streams.size(); i++)
        sids[i] = streams[i].streamID;
    if (correlators_.find(sids) == correlators_.end()) {
        FILE_LOG(logERROR) << "Tried to transfer invalid correlator.";
        throw X6_INVALID_CHANNEL;
    }
    // Don't copy more than we have
    if (length < correlators_[sids].get_buffer_size()) {
        FILE_LOG(logERROR) << "Not enough memory allocated in buffer to transfer correlator.";
    }
    correlators_[sids].snapshot_variance(buffer);
}

int X6_1000::get_buffer_size(vector<QDSPStream> & streams) {
    vector<uint16_t> sids(streams.size());
    for (size_t i = 0; i < streams.size(); i++)
        sids[i] = streams[i].streamID;
    if (streams.size() == 1) {
        return accumulators_[sids[0]].get_buffer_size();
    } else {
        return correlators_[sids].get_buffer_size();
    }
}

int X6_1000::get_variance_buffer_size(vector<QDSPStream> & streams) {
    vector<uint16_t> sids(streams.size());
    for (size_t i = 0; i < streams.size(); i++)
        sids[i] = streams[i].streamID;
    if (streams.size() == 1) {
        return accumulators_[sids[0]].get_variance_buffer_size();
    } else {
        return correlators_[sids].get_variance_buffer_size();
    }
}

void X6_1000::initialize_accumulators() {
    for (auto kv : activeQDSPStreams_) {
        accumulators_[kv.first] = Accumulator(kv.second, recordLength_, numSegments_, waveforms_);
    }
}

void X6_1000::initialize_correlators() {
    vector<uint16_t> streamIDs = {};
    vector<QDSPStream> streams = {};

    // create all n-body correlators
    for (int n = 2; n < MAX_N_BODY_CORRELATIONS; n++) {
        streamIDs.resize(n);
        streams.resize(n);

        for (auto c : combinations(resultChans_.size(), n)) {
            for (int i = 0; i < n; i++) {
                streamIDs[i] = resultChans_[c[i]];
                streams[i] = activeQDSPStreams_[streamIDs[i]];
            }
            correlators_[streamIDs] = Correlator(streams, numSegments_, waveforms_);
        }
    }
}
/****************************************************************************
 * Event Handlers
 ****************************************************************************/

 void  X6_1000::HandleDisableTrigger(OpenWire::NotifyEvent & /*Event*/) {
    //Seems to be called when AtConfigure is called on the trigger module
    FILE_LOG(logDEBUG) << "X6_1000::HandleDisableTrigger";
    module_.Input().Trigger().External(false);
    module_.Output().Trigger().External(false);
}


void  X6_1000::HandleExternalTrigger(OpenWire::NotifyEvent & /*Event*/) {
    //This is called when ``AtStreamStart`` is called on the trigger manager module
    // and external trigger has been set with ExternalTrigger(true) being called on the trigger module
    FILE_LOG(logDEBUG) << "X6_1000::HandleExternalTrigger";
    module_.Input().Trigger().External(true);
    module_.Output().Trigger().External(true);
}


void  X6_1000::HandleSoftwareTrigger(OpenWire::NotifyEvent & /*Event*/) {
    FILE_LOG(logDEBUG) << "X6_1000::HandleSoftwareTrigger";
}

void X6_1000::HandleBeforeStreamStart(OpenWire::NotifyEvent & /*Event*/) {
}

void X6_1000::HandleAfterStreamStart(OpenWire::NotifyEvent & /*Event*/) {
    FILE_LOG(logINFO) << "Analog I/O started";
    timer_.Enabled(true);
}

void X6_1000::HandleAfterStreamStop(OpenWire::NotifyEvent & /*Event*/) {
    FILE_LOG(logINFO) << "Analog I/O stopped";
    // Disable external triggering initially
    module_.Input().SoftwareTrigger(false);
    module_.Input().Trigger().External(false);
    VMPs_[0].Flush();
    VMPs_[1].Flush();
    VMPs_[2].Flush();
}

void X6_1000::HandleDataAvailable(Innovative::VitaPacketStreamDataEvent & Event) {
  if (!isRunning_) return;

  // create a buffer to receive the data
  VeloBuffer buffer;
  Event.Sender->Recv(buffer);

  AlignedVeloPacketExQ::Range InVelo(buffer);
  size_t ct = 0;
  unsigned int * pos = InVelo.begin();
  FILE_LOG(logDEBUG3) << "[HandleDataAvailable] Velo packet of size " << buffer.SizeInInts() << " contains...";
  while (ct < buffer.SizeInInts()){
      VitaHeaderDatagram vh_dg(pos+ct);
      double timeStamp = vh_dg.TS_Seconds() + 5e-9*vh_dg.TS_FSeconds();
      FILE_LOG(logDEBUG3) << "\t stream ID = " << hexn<4> << vh_dg.StreamId() <<
              " with size " << vh_dg.PacketSize() <<
              "; packet count = " << std::dec << vh_dg.PacketCount() <<
              " at timestamp " << timeStamp;
      ct += vh_dg.PacketSize();
  }

  // broadcast to all VMPs
  for (auto & vmp : VMPs_) {
      vmp.Append(buffer);
      vmp.Parse();
  }

  if (check_done()) {
      FILE_LOG(logINFO) << "check_done() returned true. Stopping...";
      stop();
  }
}

void X6_1000::VMPDataAvailable(Innovative::VeloMergeParserDataAvailable & Event, STREAM_T streamType) {
    if (!isRunning_) {
        return;
    }
    // StreamID is now encoded in the PeripheralID of the VMP Vita buffer
    // PeripheralID is just the order of the streamID in the filter
    PacketBufferHeader header(Event.Data);
    uint16_t sid;

    switch (streamType) {
        case PHYSICAL:
            sid = physChans_[header.PeripheralId()];
            break;
        case DEMOD:
            sid = virtChans_[header.PeripheralId()];
            break;
        case RESULT:
            sid = resultChans_[header.PeripheralId()];
            break;
    }

    // interpret the data as 16 or 32-bit integers depending on the channel type
    ShortDG sbufferDG(Event.Data);
    IntegerDG ibufferDG(Event.Data);
    switch (streamType) {
        case PHYSICAL:
        case DEMOD:
            FILE_LOG(logDEBUG3) << "[VMPDataAvailable] buffer SID = " << hexn<4> << sid << "; buffer.size = " << std::dec << sbufferDG.size() << " samples";
            // accumulate the data in the appropriate channel
            if (accumulators_[sid].recordsTaken < numRecords_) {
                accumulators_[sid].accumulate(sbufferDG);
            }
            break;
        case RESULT:
            FILE_LOG(logDEBUG3) << "[VMPDataAvailable] buffer SID = " << hexn<4> << sid << "; buffer.size = " << std::dec << ibufferDG.size() << " samples";
            // accumulate the data in the appropriate channel
            if (accumulators_[sid].recordsTaken < numRecords_) {
                accumulators_[sid].accumulate(ibufferDG);
                // correlate with other result channels
                for (auto & kv : correlators_) {
                    if (std::find(kv.first.begin(), kv.first.end(), sid) != kv.first.end()) {
                        kv.second.accumulate(sid, ibufferDG);
                    }
                }
            }
            break;
    }
}

bool X6_1000::check_done() {
    for (auto & kv : accumulators_) {
        FILE_LOG(logDEBUG2) << "Channel " << hexn<4> << kv.first << " has taken " << std::dec << kv.second.recordsTaken << " records.";
    }
    for (auto & kv : accumulators_) {
            if (kv.second.recordsTaken < numRecords_) {
            return false;
        }
    }
    return true;
}

void X6_1000::write_pulse_waveform(unsigned pg, vector<double>& wf){
    FILE_LOG(logDEBUG1) << "Writing waveform of length " << wf.size() << " to PG " << pg;
    //Waveform length should be multiple of four and less than 16384
    if (((wf.size() % 4) != 0) || (wf.size() > 16384)){
        FILE_LOG(logERROR) << "invalid waveform length " << wf.size();
        throw X6_INVALID_WF_LEN;
    }

    auto range_check = [](double val){
        const double maxVal = 1 - 1.0/(1 << 15);
        const double minVal = -1.0;
        if ((val > maxVal) || (val < minVal)){
            FILE_LOG(logERROR) << "waveform value out of range: " << val;
            throw X6_WF_OUT_OF_RANGE;
        }
    };

    //Loop through pairs, convert to 16bit integer, stack into a uint32_t
    for (size_t ct = 0; ct < wf.size(); ct+=2) {
        range_check(wf[ct]);
        int32_t fixedValA = wf[ct]*(1<<15);
        range_check(wf[ct+1]);
        int32_t fixedValB = wf[ct+1]*(1<<15);
        uint32_t stackedVal = (fixedValB << 16) | (fixedValA & 0x0000ffff); // signed to unsigned is defined modulo 2^n in the standard
        FILE_LOG(logDEBUG2) << "Writing waveform values " << wf[ct] << "(" << hexn<4> << fixedValA << ") and " <<
                    wf[ct+1] << "(" << hexn<4> << fixedValB << ") as " << hexn<8> << stackedVal;
        write_wishbone_register(BASE_PG[pg], 9, ct/2); // address
        write_wishbone_register(BASE_PG[pg], 10, stackedVal); //data
    }
}

double X6_1000::read_pulse_waveform(unsigned pg, uint16_t addr){
    FILE_LOG(logDEBUG1) << "Reading PG " << pg << " waveform at address " << addr;
    write_wishbone_register(BASE_PG[pg], 9, addr/2); // address is in 32bit words
    uint32_t stackedVal = read_wishbone_register(BASE_PG[pg], 10);

    //If the address is even or odd take the upper/lower 16bits
    //The conversion from unsigned to signed is not guaranteed to keep the bit pattern
    //However for gcc using two's complement it does
    //See http://stackoverflow.com/a/4219558 and http://stackoverflow.com/q/13150449
    int16_t fixedVal = ((addr % 2) == 0) ? (stackedVal & 0x0000ffff) : (stackedVal >> 16);

    //Convert back to -1 to 1 float
    return static_cast<double>(fixedVal)/(1<<15);
}

void X6_1000::HandleTimer(OpenWire::NotifyEvent & /*Event*/) {
    // FILE_LOG(logDEBUG) << "X6_1000::HandleTimer";
    trigger_.AtTimerTick();
}

void X6_1000::write_wishbone_register(uint32_t baseAddr, uint32_t offset, uint32_t data) {
     // Initialize WishboneAddress Space for APS specific firmware
    Innovative::AddressingSpace & logicMemory = Innovative::LogicMemorySpace(const_cast<X6_1000M&>(module_));
    Innovative::WishboneBusSpace WB_X6 = Innovative::WishboneBusSpace(logicMemory, baseAddr);
    //Register.Value is defined as an ii32 in HardwareRegister_Mb.cpp and ii32 is typedefed as unsigend in DataTypes_Mb.h
    Innovative::Register reg = Register(WB_X6, offset);
    reg.Value(data);
}

uint32_t X6_1000::read_wishbone_register(uint32_t baseAddr, uint32_t offset) const {
    Innovative::AddressingSpace & logicMemory = Innovative::LogicMemorySpace(const_cast<X6_1000M&>(module_));
    Innovative::WishboneBusSpace WB_X6 = Innovative::WishboneBusSpace(logicMemory, baseAddr);
    //Register.Value is defined as an ii32 in HardwareRegister_Mb.cpp and ii32 is typedefed as unsigend in DataTypes_Mb.h
    Innovative::Register reg = Register(WB_X6, offset);
    return reg.Value();
}

void X6_1000::write_dsp_register(unsigned instance, uint32_t offset, uint32_t data) {
    write_wishbone_register(BASE_DSP[instance], offset, data);
}

uint32_t X6_1000::read_dsp_register(unsigned instance, uint32_t offset) const {
    return read_wishbone_register(BASE_DSP[instance], offset);
}
