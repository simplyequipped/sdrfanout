CXX = c++
CXXFLAGS = -O2 -std=c++17 -Wall
SOAPY_LIBS = -lSoapySDR -lpthread

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

# Full producer loop via the synthetic capture.
test/test_loop: test/test_loop.cc run.cc dsp.cc framer.cc synth.cc util.cc
	$(CXX) $(CXXFLAGS) -I. test/test_loop.cc run.cc dsp.cc framer.cc synth.cc util.cc -o $@ -lm

test: test/test_util test/test_dsp test/test_stream test/test_loop
	./test/test_util
	@echo
	./test/test_dsp
	@echo
	./test/test_stream
	@echo
	./test/test_loop

clean:
	rm -f sdrfanout test/test_util test/test_dsp test/test_stream test/test_loop

.PHONY: test clean
