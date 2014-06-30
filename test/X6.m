classdef X6 < hgsetget
    
    properties (Constant)
        library_path = '../build/';
    end
    
    properties
        samplingRate = 1000;
        triggerSource
        reference
        bufferSize = 0;
        is_open = 0;
        deviceID = 0;
    end
    
    properties(Constant)
        DECIM_FACTOR = 4;
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
        
        function val = enable_stream(obj, physChan, demodChan)
            val = obj.libraryCall('enable_stream', physChan, demodChan);
        end
        
        function val = disable_stream(obj, physChan, demodChan)
            val = obj.libraryCall('disable_stream', physChan, demodChan);
        end
        
        function val = set_averager_settings(obj, recordLength, numSegments, waveforms, roundRobins)
            val = obj.libraryCall('set_averager_settings', recordLength, numSegments, waveforms, roundRobins);
            obj.bufferSize = recordLength * numSegments;
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

        function wf = transfer_waveform(obj, physChan, demodChan)
            % possibly more efficient to pass a libpointer, but this is easiest for now
            if demodChan == 0
                bufSize = obj.bufferSize;
            else
                bufSize = 2*obj.bufferSize/obj.DECIM_FACTOR;
            end
            wfPtr = libpointer('doublePtr', zeros(bufSize, 1, 'double'));
            success = obj.libraryCall('transfer_waveform', physChan, demodChan, wfPtr, bufSize);
            assert(success == 0, 'transfer_waveform failed');

            if demodChan == 0
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

        
        function val = getLogicTemperature(obj)
            % get temprature using method one based on Malibu Objects
            val = obj.libraryCall('get_logic_temperature', 0);
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
            
            x6.setDebugLevel(6);
            
            x6.connect(0);
            
            if (~x6.is_open)
                error('Could not open aps')
            end

            fprintf('current logic temperature = %.1f\n', x6.getLogicTemperature());
            
            fprintf('current PLL frequency = %.2f GHz\n', x6.samplingRate/1e9);
            fprintf('Setting clock reference to external\n');
            x6.reference = 'external';
            
            x6.enable_stream(1, 0);
            x6.enable_stream(1, 1);
            x6.enable_stream(1, 2);
            
            phase_increment = 4/50;
            fprintf('Setting NCO phase increment to %.3f\n', phase_increment);
            x6.writeRegister(hex2dec('700'), 16, round(4 * phase_increment * 2^16));
            x6.writeRegister(hex2dec('700'), 17, round(4 * phase_increment * 2^16));
            
            x6.writeRegister(hex2dec('700'), 32, hex2dec('00020100'));
            x6.writeRegister(hex2dec('700'), 33, hex2dec('00020110'));
            x6.writeRegister(hex2dec('700'), 34, hex2dec('00020120'));
 
            fprintf('setting averager parameters to record 10 segments of 2048 samples\n');
            x6.set_averager_settings(2048, 50, 1, 1);
%             x6.writeRegister(hex2dec('700'), 0, 1024);
%             x6.writeRegister(hex2dec('700'), 1, 256);
%             x6.writeRegister(hex2dec('700'), 2, 256);

            fprintf('Acquiring\n');
            x6.acquire();

            success = x6.wait_for_acquisition(1);
            fprintf('Wait for acquisition returned %d\n', success);

            fprintf('Stopping\n');
            x6.stop();

            fprintf('Transferring waveform channels 1 and 2\n');
            wf1 = x6.transfer_waveform(1,0);
            wf2 = x6.transfer_waveform(1,1);
            wf3 = x6.transfer_waveform(1,2);
            figure();
            subplot(3,1,1);
            plot(wf1);
            title('Raw Channel 1');
            subplot(3,1,2);
            plot(real(wf2), 'b');
            hold on
            plot(imag(wf2), 'r');
            title('Virtual Channel 1');
            subplot(3,1,3);
            plot(real(wf3), 'b');
            hold on
            plot(imag(wf3), 'r');
            title('Virtual Channel 2');
            
            x6.disconnect();
            unloadlibrary('libx6adc')
        end
        
    end
    
end