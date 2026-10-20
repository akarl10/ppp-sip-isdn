# Makefile for ppp-sip-isdn
#
PJSIP_DIR=pjproject
PKG_CONFIG_PATH=pjsip.install/lib/pkgconfig

APP = ppp-sip-isdn
SRC = ppp-sip-isdn.c clearmode_codec.c hdlc-bitstream.c
HLP = ppp-helper
HLP_SRC = ppp-helper.c

# Adjust if your PJSIP install lives elsewhere
PJSIP_PREFIX = ./pjsip.install
CFLAGS = -I$(PJSIP_PREFIX)/include -O2 -Wall -DSECURE_PPP_HELPER=\"/usr/local/libexec/ppp-sip-isdn/ppp-helper\"

all: $(APP)

$(APP): $(SRC) $(HLP_SRC) $(PKG_CONFIG_PATH)/libpjproject.pc
	$(CC) $(CFLAGS) -o $(APP) $(SRC) $(LDFLAGS) `PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --static --cflags --libs libpjproject`
	$(CC) $(CFLAGS) -o $(HLP) $(HLP_SRC)

$(PKG_CONFIG_PATH)/libpjproject.pc:
	(cd $(PJSIP_DIR); [ -f ./config.status ] || CFLAGS="-DPJSUA_MAX_CALLS=512 -DPJ_HAS_IPV6=1" ./configure --prefix=`pwd`/../pjsip.install --disable-video --disable-sound)
	$(MAKE) -C $(PJSIP_DIR) && \
	$(MAKE) -C $(PJSIP_DIR) install


test: tests/test_hdlc
	./tests/test_hdlc

tests/test_hdlc: tests/test_hdlc.c hdlc-bitstream.c hdlc-bitstream.h
	$(CC) -O2 -Wall -o $@ tests/test_hdlc.c hdlc-bitstream.c

# End-to-end test: ppp-sip-isdn built against a fake pppd (see tests/test_e2e.sh)
tests/fake_pppd: tests/fake_pppd.c
	$(CC) -O2 -Wall -o $@ $<

tests/ppp-sip-isdn-test: $(SRC) tests/fake_pppd $(PKG_CONFIG_PATH)/libpjproject.pc
	$(CC) -I$(PJSIP_PREFIX)/include -O2 -Wall -DSECURE_PPP_HELPER=\"$(CURDIR)/tests/fake_pppd\" -o $@ $(SRC) $(LDFLAGS) `PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --static --cflags --libs libpjproject`

# Single lines must be lossless. The 8-line runs only require 80% of the frames to
# arrive: pjmedia's jitter buffer occasionally runs empty and conceals a 20ms frame,
# which shows up as lost HDLC data (more likely around call teardown).
test-e2e: tests/ppp-sip-isdn-test
	./tests/test_e2e.sh 1
	./tests/test_e2e.sh 1 --pcma
	MIN_PCT=80 ./tests/test_e2e.sh 8
	MIN_PCT=80 FRAMES=150 KILL=half ./tests/test_e2e.sh 8   # half of the lines drop mid-exchange

.PHONY: all test test-e2e

#clean:
#    rm -f $(APP)

