# Freeswitch howto

This directory contains a modified version of the deprecated mod_clearmode that implements CLEARMODE transcoding/repacketization to handle different ptime values between call legs.

To build it put the directory mod_clearmode in the Freeswitch source tree under src/mod/codecs/mod_clearmode
and enable the codec in modules.conf at the root of the Freeswitch source tree by adding
```
codecs/mod_clearmode
```

don't forget loading the module by adding a line in your modules.conf.xml file:

```xml
<load module="mod_clearmode"/>
```

a XML dialplan snipped modified from the default one that worked for me is the following:

```xml
<extension name="Local_Extension">
    <condition field="destination_number" expression="^(10[012][0-9])$">
        <action application="export" data="dialed_extension=$1"/>
        <!-- bind_meta_app can have these args <key> [a|b|ab] [a|b|o|s] <app> -->
        <action application="set" data="ringback=${us-ring}"/>
        <action application="set" data="transfer_ringback=$${hold_music}"/>
        <action application="set" data="call_timeout=30"/>
        <!-- <action application="set" data="sip_exclude_contact=${network_addr}"/> -->
        <action application="set" data="hangup_after_bridge=true"/>
        <!--<action application="set" data="continue_on_fail=NORMAL_TEMPORARY_FAILURE,USER_BUSY,NO_ANSWER,TIMEOUT,NO_ROUTE_DESTINATION"/> -->
        <action application="set" data="continue_on_fail=true"/>
        <action application="hash" data="insert/${domain_name}-call_return/${dialed_extension}/${caller_id_number}"/>
        <action application="hash" data="insert/${domain_name}-last_dial_ext/${dialed_extension}/${uuid}"/>
        <action application="set" data="called_party_callgroup=${user_data(${dialed_extension}@${domain_name} var callgroup)}"/>
        <action application="hash" data="insert/${domain_name}-last_dial_ext/${called_party_callgroup}/${uuid}"/>
        <action application="hash" data="insert/${domain_name}-last_dial_ext/global/${uuid}"/>
        <action application="hash" data="insert/${domain_name}-last_dial/${called_party_callgroup}/${uuid}"/>
        </condition>
    <condition field="${switch_r_sdp}" expression="CLEARMODE/8000" break="never">
        <action application="export" data="absolute_codec_string=CLEARMODE"/>
        <action application="export" data="dtmf_type=none"/>
        <anti-action application="set" data="fax_enable_t38=true"/>
        <anti-action application="export" data="fax_enable_t38=true"/>
        <anti-action application="export" data="t38_passthru=true"/>
        <anti-action application="set" data="t38_passthru=true"/>
    </condition>
    <condition>
        <action application="export" data="origination_callee_id_name=${user_data(${dialed_extension}@${domain_name} var effective_caller_id_name)}"/>
        <action application="bridge" data="user/${dialed_extension}@${domain_name}"/>
        <action application="answer"/>
        <action application="sleep" data="1000"/>
        <action application="export" data="default_language=en"/>
        <action application="voicemail" data="default ${domain_name} ${dialed_extension}"/>
    </condition>
</extension>
```
the relevant part is `<condition field="${switch_r_sdp}" expression="CLEARMODE/8000" break="never">`. you may not need anti-action, I just use this to allow t38 fax renegotiation on my extensions

You can simply register your dialin server with freeswitch like this:

```bash
#freeswitch domain is freeswitch.local in this example
env SIP_PASS=secret ppp-sip-isdn --id sip:1001@freeswitch.local --reg sip:freeswitch.local --user 1001 --bind 15060
```

the client dialing works like this
```bash
#freeswitch domain is freeswitch.local in this example
env SIP_PASS=secret ppp-sip-isdn --id sip:1002@freeswitch.local --reg sip:freeswitch.local --user 1002 --dial sip:1001@freeswitch.local --bind 25060
```

using `--bind` is recommended to not have to wait old registrations to timeout on the freeswitch side, but not strictly required

# using vanilla freeswitch
if you want to pass CLEARMODE without the need for the mod_clearmode (upstream does not have this anymore), bypass_media or proxy_media might work, but I had no luck, at least with proxy_media.
I have not tried bybass_media because on my test system one host was on ipv6, the other ipv4 so that would never work in bypass_media mode