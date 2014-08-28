% MATLAB wrapper for the X6 driver.
%
% Usage notes: The channelizer creates multiple data streams per physical
% input channel. These are indexed by a 3-parameter label (a,b,c), where a
% is the 1-indexed physical channel, b is the 0-indexed virtual channel
% (b=0 is the raw stream, b>1 are demodulated streams, and c indicates
% demodulated (c=0) or demodulated and integrated (c = 1).

% Original authors: Blake Johnson and Colm Ryan
% Date: August 25, 2014

% Copyright 2014 Raytheon BBN Technologies
%
% Licensed under the Apache License, Version 2.0 (the "License");
% you may not use this file except in compliance with the License.
% You may obtain a copy of the License at
%
%     http://www.apache.org/licenses/LICENSE-2.0
%
% Unless required by applicable law or agreed to in writing, software
% distributed under the License is distributed on an "AS IS" BASIS,
% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
% See the License for the specific language governing permissions and
% limitations under the License.

classdef X6 < hgsetget
    
    properties (Constant)
        library_path = '../build/';
    end
    
    properties
        samplingRate = 1e9;
        triggerSource
        reference
        is_open = 0;
        deviceID = 0;
    end
    
    properties(Constant)
        DECIM_FACTOR = 4;
        DSP_WB_OFFSET = [hex2dec('2000'), hex2dec('2100')];
        SPI_ADDRS = containers.Map({'adc0', 'adc1', 'dac0', 'dac1'}, {16, 18, 144, 146});
    end
    
    methods
        function obj = X6()
            obj.load_library();
        end

        function val = connect(obj, id)
            val = calllib('libx6adc', 'connect_by_ID', id);
            if (val == 0)
                obj.is_open = 1;
                obj.deviceID = id;
            end
        end

        function val = disconnect(obj)
            val = obj.libraryCall('disconnect');
            obj.is_open = 0;
        end

        function delete(obj)
            if (obj.is_open)
                obj.disconnect();
            end
        end

        function val = num_devices(obj)
            val = calllib('libx6adc', 'get_num_devices');
        end

        function val = init(obj)
            val = obj.libraryCall('initX6');
        end

        function val = get.samplingRate(obj)
            val = obj.libraryCall('get_sampleRate');
        end

        function set.samplingRate(obj, rate)
            val = obj.libraryCall('set_sampleRate', rate);
        end

        function val = get.triggerSource(obj)
            val = obj.libraryCall('get_trigger_source');
        end

        function set.triggerSource(obj, source)
            obj.libraryCall('set_trigger_source', source);
        end
        
        function set.reference(obj, reference)
            valMap = containers.Map({'int', 'internal', 'ext', 'external'}, {0, 0, 1, 1});
            obj.libraryCall('set_reference', valMap(lower(reference)));
        end
        
        function val = get.reference(obj)
            val = obj.libraryCall('get_reference_source');
        end
        
        function val = enable_stream(obj, a, b, c)
            val = obj.libraryCall('enable_stream', a, b, c);
        end
        
        function val = disable_stream(obj, a, b, c)
            val = obj.libraryCall('disable_stream', a, b, c);
        end
        
        function val = set_averager_settings(obj, recordLength, numSegments, waveforms, roundRobins)
            val = obj.libraryCall('set_averager_settings', recordLength, numSegments, waveforms, roundRobins);
        end

        function val = acquire(obj)
            val = obj.libraryCall('acquire');
        end

        function val = wait_for_acquisition(obj, timeout)
            val = obj.libraryCall('wait_for_acquisition', timeout);
        end

        function val = stop(obj)
            val = obj.libraryCall('stop');
        end

        function wf = transfer_waveform(obj, a, b, c)
            bufSize = obj.libraryCall('get_buffer_size', a, b, c);
            wfPtr = libpointer('doublePtr', zeros(bufSize, 1, 'double'));
            success = obj.libraryCall('transfer_waveform', a, b, c, wfPtr, bufSize);
            assert(success == 0, 'transfer_waveform failed');

            if b == 0 % physical channel
                wf = wfPtr.Value;
            else
                wf = wfPtr.Value(1:2:end) +1i*wfPtr.Value(2:2:end);
            end
        end
        
        function val = writeRegister(obj, addr, offset, data)
            % get temprature using method one based on Malibu Objects
            val = obj.libraryCall('write_register', addr, offset, data);
        end

        function val = readRegister(obj, addr, offset)
            % get temprature using method one based on Malibu Objects
            val = obj.libraryCall('read_register', addr, offset);
        end
        
        function write_spi(obj, chip, addr, data)
           %read flag is low so just address
           val = bitshift(addr, 16) + data;
           obj.writeRegister(hex2dec('0800'), obj.SPI_ADDRS(chip), val);
        end

        function val = read_spi(obj, chip, addr)
           %read flag is high
           val = bitshift(1, 28) + bitshift(addr, 16);
           obj.writeRegister(hex2dec('0800'), obj.SPI_ADDRS(chip), val);
           val = int32(obj.readRegister(hex2dec('0800'), obj.SPI_ADDRS(chip)+1));
           assert(bitget(val, 32) == 1, 'Oops! Read valid flag was not set!');
        end
        
        function val = getLogicTemperature(obj)
            % get temprature using method one based on Malibu Objects
            val = obj.libraryCall('get_logic_temperature', 0);
        end
        
        function set_nco_frequency(obj, a, b, freq)
            phase_increment = 4 * freq/obj.samplingRate; % NCO runs at quarter rate
            obj.writeRegister(X6.DSP_WB_OFFSET(a), 16+(b-1), round(1 * phase_increment * 2^18));
        end
        
        function write_kernel(obj, phys, demod, kernel)
            obj.writeRegister(obj.DSP_WB_OFFSET(phys), 24+demod-1, length(kernel));
            kernel = int32(kernel * (2^15-1)); % scale up to integers
            for ct = 1:length(kernel)
                obj.writeRegister(obj.DSP_WB_OFFSET(phys), 48+2*(demod-1), ct-1);
                obj.writeRegister(obj.DSP_WB_OFFSET(phys), 48+2*(demod-1)+1, bitshift(real(kernel(ct)), 16) + bitand(imag(kernel(ct)), hex2dec('FFFF')));
            end
        end
        
        function set_threshold(obj, a, b, threshold)
            obj.writeRegister(X6.DSP_WB_OFFSET(a), 56+(b-1), int32(threshold));
        end
        
        %Instrument meta-setter that sets all parameters
        function setAll(obj, settings)
            obj.settings = settings;
            fields = fieldnames(settings);
            for tmpName = fields'
                switch tmpName{1}
                    case 'horizontal'
                        % Skip for now. Eventually this is where you'd want
                        % to pass thru trigger delay
                    case 'averager'
                        obj.set_averager_settings( ...
                            settings.averager.recordLength, ...
                            settings.averager.nbrSegments, ...
                            settings.averager.nbrWaveforms, ...
                            settings.averager.nbrRoundRobins);
                    otherwise
                        if ismember(tmpName{1}, methods(obj))
                            feval(['obj.' tmpName{1}], settings.(tmpName{1}));
                        elseif ismember(tmpName{1}, properties(obj))
                            obj.(tmpName{1}) = settings.(tmpName{1});
                        end
                end
            end
        end
    end
    
    methods (Access = protected)
        % overide APS load_library
        function load_library(obj)
            %Helper functtion to load the platform dependent library
            switch computer()
                case 'PCWIN64'
                    libfname = 'libx6adc.dll';
                    libheader = '../src/libx6adc.matlab.h';
                    %protoFile = @obj.libaps64;
                otherwise
                    error('Unsupported platform.');
            end
            % build library path and load it if necessary
            if ~libisloaded('libx6adc')
                loadlibrary(fullfile(obj.library_path, libfname), libheader );
            end
        end

        function val = libraryCall(obj,func,varargin)
            %Helper function to pass through to calllib with the X6 device ID first 
            if ~(obj.is_open)
                error('X6:libraryCall','X6 is not open');
            end
                        
            if size(varargin,2) == 0
                val = calllib('libx6adc', func, obj.deviceID);
            else
                val = calllib('libx6adc', func, obj.deviceID, varargin{:});
            end
        end
    end
    
    methods (Static)
        
        function setDebugLevel(level)
            % sets logging level in libx6.log
            % level = {logERROR=0, logWARNING, logINFO, logDEBUG, logDEBUG1, logDEBUG2, logDEBUG3, logDEBUG4}
            calllib('libx6adc', 'set_logging_level', level);
        end
        
        function UnitTest()
            
            fprintf('BBN X6-1000 Test Executable\n')
            
            x6 = X6();
            
            x6.setDebugLevel(8);
            
            x6.connect(0);
            
            if (~x6.is_open)
                error('Could not open X6')
            end

            fprintf('current logic temperature = %.1f\n', x6.getLogicTemperature());
            
            fprintf('current PLL frequency = %.2f GHz\n', x6.samplingRate/1e9);
            fprintf('Setting clock reference to external\n');
            x6.reference = 'external';
            
            fprintf('Enabling streams\n');
            for phys = 1:2
                x6.enable_stream(phys, 0, 0); % the raw stream
                for demod = 1:2
                    for result = 0:1
                        x6.enable_stream(phys, demod, result);
                    end
                end
            end
            
            fprintf('Setting NCO phase increments\n');
            x6.set_nco_frequency(1, 1, 10e6);
            x6.set_nco_frequency(1, 2, 30e6);
            x6.set_nco_frequency(2, 1, 20e6);
            x6.set_nco_frequency(2, 2, 40e6);
            
            fprintf('Writing integration kernels\n');
            x6.write_kernel(1, 1, ones(128,1));
            x6.write_kernel(1, 2, ones(128,1));
            x6.write_kernel(2, 1, ones(128,1));
            x6.write_kernel(2, 2, ones(128,1));
            
            fprintf('Writing decision engine thresholds\n');
            x6.set_threshold(1, 1, 4000);
            x6.set_threshold(1, 2, 4000);
            x6.set_threshold(2, 1, 4000);
            x6.set_threshold(2, 2, 4000);
            
            fprintf('setting averager parameters to record 9 segments of 2048 samples\n');
            x6.set_averager_settings(2048, 9, 1, 1);

            fprintf('Acquiring\n');
            x6.acquire();

            success = x6.wait_for_acquisition(0.1);
            fprintf('Wait for acquisition returned %d\n', success);

            fprintf('Stopping\n');
            x6.stop();

            fprintf('Transferring waveforms\n');
            numDemodChan = 2;
            wfs = cell(numDemodChan+1,1);
            for ct = 0:numDemodChan
                wfs{ct+1} = x6.transfer_waveform(1,ct,0);
            end
            figure();
            subplot(numDemodChan+1,1,1);
            plot(wfs{1});
            title('Raw Channel 1');

            for ct = 1:numDemodChan
                subplot(numDemodChan+1,1,ct+1);
                plot(real(wfs{ct+1}), 'b');
                hold on
                plot(imag(wfs{ct+1}), 'r');
                title(sprintf('Virtual Channel %d',ct));
            end
            
            
            for ct = 0:numDemodChan
                wfs{ct+1} = x6.transfer_waveform(2,ct,0);
            end
            figure();
            subplot(numDemodChan+1,1,1);
            plot(wfs{1});
            title('Raw Channel 2');

            for ct = 1:numDemodChan
                subplot(numDemodChan+1,1,ct+1);
                plot(real(wfs{ct+1}), 'b');
                hold on
                plot(imag(wfs{ct+1}), 'r');
                title(sprintf('Virtual Channel %d',ct));
            end

            x6.disconnect();
            unloadlibrary('libx6adc')
        end
        
    end
    
end