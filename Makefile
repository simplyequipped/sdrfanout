CXX = c++
CXXFLAGS = -O3 -std=c++17 -Wall -ffast-math -pthread
SOAPY_LIBS = -lSoapySDR

# The sdrfanout binary. soapy.cc holds the SoapySDR Capture implementation.
SRC = main.cc util.cc synth.cc soapy.cc dsp.cc framer.cc run.cc

sdrfanout: $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o sdrfanout $(SOAPY_LIBS)

# Unit tests for the DSP / framing / util logic. Binaries land in test/.
test/test_util: test/test_util.cc util.cc
	$(CXX) $(CXXFLAGS) -I. test/test_util.cc util.cc -o $@ -lm

test/test_dsp: test/test_dsp.cc dsp.cc
	$(CXX) $(CXXFLAGS) -I. test/test_dsp.cc dsp.cc -o $@ -lm

test/test_stream: test/test_stream.cc dsp.cc framer.cc
	$(CXX) $(CXXFLAGS) -I. test/test_stream.cc dsp.cc framer.cc -o $@ -lm

# Signal level reporting: dBFS accuracy, mode independence, reset behaviour.
test/test_level: test/test_level.cc dsp.cc
	$(CXX) $(CXXFLAGS) -I. test/test_level.cc dsp.cc -o $@ -lm

# Every combination of channel count, mode, format and output rate, over a range
# of input rates: stream structure and data accuracy in each one.
test/test_input_output_combos: test/test_input_output_combos.cc dsp.cc framer.cc
	$(CXX) $(CXXFLAGS) -I. test/test_input_output_combos.cc dsp.cc framer.cc -o $@ -lm

# Full producer loop via the synthetic capture.
test/test_loop: test/test_loop.cc run.cc dsp.cc framer.cc synth.cc util.cc
	$(CXX) $(CXXFLAGS) -I. test/test_loop.cc run.cc dsp.cc framer.cc synth.cc util.cc -o $@ -lm

# Multi-thread fan-out must be byte-identical to single-thread.
test/test_threads: test/test_threads.cc run.cc dsp.cc framer.cc synth.cc util.cc
	$(CXX) $(CXXFLAGS) -I. test/test_threads.cc run.cc dsp.cc framer.cc synth.cc util.cc -o $@ -lm

test: test/test_util test/test_dsp test/test_stream test/test_level \
      test/test_input_output_combos test/test_loop test/test_threads
	./test/test_util
	@echo
	./test/test_dsp
	@echo
	./test/test_stream
	@echo
	./test/test_level
	@echo
	./test/test_input_output_combos
	@echo
	./test/test_loop
	@echo
	./test/test_threads

clean:
	rm -f sdrfanout test/test_util test/test_dsp test/test_stream test/test_level \
	      test/test_input_output_combos test/test_loop test/test_threads

.PHONY: test clean
