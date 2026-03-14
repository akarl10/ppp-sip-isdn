# Makefile for ppp-sip-isdn
#
PJSIP_DIR=pjproject
PKG_CONFIG_PATH=pjsip.install/lib/pkgconfig

APP = ppp-sip-isdn
SRC = ppp-sip-isdn.c clearmode_codec.c

# Adjust if your PJSIP install lives elsewhere
PJSIP_PREFIX = ./pjsip.install
CFLAGS = -I$(PJSIP_PREFIX)/include -O2 -Wall

all: $(APP)

$(APP): $(SRC) $(PKG_CONFIG_PATH)/libpjproject.pc
	$(CC) $(CFLAGS) -o $(APP) $(SRC) $(LDFLAGS) `PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --static --cflags --libs libpjproject`

$(PKG_CONFIG_PATH)/libpjproject.pc:
	(cd $(PJSIP_DIR); [ -f ./config.status ] || CFLAGS="-DPJSUA_MAX_CALLS=512 -DPJ_HAS_IPV6=1" ./configure --prefix=`pwd`/../pjsip.install --disable-video --disable-sound)
	$(MAKE) -C $(PJSIP_DIR) && \
	$(MAKE) -C $(PJSIP_DIR) install


#clean:
#    rm -f $(APP)

