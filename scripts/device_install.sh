#!/bin/sh
# Runs on the device, detached from the ssh session, so a dropped USB link
# cannot interrupt the dpkg transaction halfway through.
echo "RUNNING" > /tmp/dpkg.out
dpkg -i /tmp/TrumpGoGo.deb >> /tmp/dpkg.out 2>&1
echo "RC=$?" >> /tmp/dpkg.out
uicache_ok=0
if [ -x /usr/bin/uicache ]; then
    if command -v su >/dev/null 2>&1; then
        su mobile -c uicache >/dev/null 2>&1 && uicache_ok=1
    else
        PLIST=/Library/LaunchDaemons/com.claw.uicache.plist
        cat > "$PLIST" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>com.claw.uicache</string>
	<key>ProgramArguments</key><array><string>/usr/bin/uicache</string></array>
	<key>UserName</key><string>mobile</string>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><false/>
	<key>StandardOutPath</key><string>/tmp/uicache.out</string>
	<key>StandardErrorPath</key><string>/tmp/uicache.out</string>
</dict>
</plist>
EOF
        launchctl load -w "$PLIST" >/dev/null 2>&1
        sleep 5
        launchctl unload "$PLIST" >/dev/null 2>&1
        rm -f "$PLIST"
        uicache_ok=1
    fi
fi
echo "UICACHE=$uicache_ok" >> /tmp/dpkg.out
sleep 1
killall -9 SpringBoard >/dev/null 2>&1
echo "DONE" >> /tmp/dpkg.out
