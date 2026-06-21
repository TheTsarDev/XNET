# Building & Deploying XNET

A step by step guide for building XNET from source on Linux. It's written for
people who have never used NXDK before, so nothing is assumed.

This takes you from a fresh Linux machine all the way to XNET running on a real
Xbox, and it also covers standing up the relay server the consoles talk to. You
don't need any prior Xbox homebrew or NXDK experience. Every command is here to
copy and paste.

There are two things to build:

1. The **XBE**, which is the XNET app that runs on the Xbox. This is built with NXDK.
2. The **relay**, a small server the consoles connect through. This is plain Node.js.

You can do them in either order, but most people set up the relay first so the
Xbox has something to connect to once it boots.

---

## What You'll Need

- A **Linux machine** to build the XBE (WSL2 on Windows works too). The commands
  below are for Ubuntu and Debian; swap `apt` for your distro's package manager
  if you're on something else.
- An **original Xbox** that runs homebrew (softmod, modchip, or TSOP). A stock
  64 MB console is perfectly fine. You do not need a RAM upgrade.
- An **FTP client** like FileZilla to copy files to the Xbox.
- For the relay, any always on **Linux host** with Node.js. A cheap VPS is ideal,
  though you can run it on your build machine for LAN testing.
- Optional, for voice and video: an **Xbox Communicator** or **HAWK Adapter**, headset and a **PS2
  EyeToy** or Xbox camera.

---

## Part 1: Set Up the Relay Server

The relay passes encrypted packets between consoles. It's deliberately tiny and
has **no third party dependencies**. It only uses Node.js's built in modules, so
there is nothing to `npm install` and no `package.json` to worry about.

### 1.1  Install Node.js

On your VPS (or any Linux host):

```bash
sudo apt update
sudo apt install -y nodejs
node --version      # should print v18.x or newer
```

If your distro ships something older than v18, grab a current build from
[nodejs.org](https://nodejs.org) or use the
[NodeSource packages](https://github.com/nodesource/distributions).

### 1.2  Create a service user and folder

It's good practice to run the relay as its own locked down user instead of root:

```bash
sudo useradd -r -s /bin/false xnet
sudo mkdir -p /opt/xnet-relay
```

### 1.3  Copy the relay files up

From your PC, send the relay script and the systemd unit to the server. Swap in
your server's address for `YOUR_VPS_IP`:

```bash
scp _src_relay/xnet-relay.js  user@YOUR_VPS_IP:/tmp/
scp _src_relay/xnet.service   user@YOUR_VPS_IP:/tmp/
```

Then on the server:

```bash
sudo mv /tmp/xnet-relay.js /opt/xnet-relay/
sudo mv /tmp/xnet.service  /etc/systemd/system/xnet.service
sudo chown -R xnet:xnet /opt/xnet-relay
```

The included `xnet.service` runs the relay as the `xnet` user, restarts it if it
ever crashes, throws all output to `/dev/null` (zero logging, which is part of
the whole point), and sandboxes the process. It expects the script at
`/opt/xnet-relay/xnet-relay.js`. If you put it somewhere else, edit the
`WorkingDirectory` and `ExecStart` lines to match.

### 1.4  Open the port

The relay listens on TCP **7777** by default:

```bash
sudo ufw allow 7777/tcp
sudo ufw status
```

### 1.5  Start it

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now xnet
sudo systemctl status xnet
```

You want to see `Active: active (running)`. There won't be any log output after
that line, and that's on purpose. The relay logs nothing.

### 1.6  Quick sanity check

From your PC:

```bash
nc YOUR_VPS_IP 7777      # or: telnet YOUR_VPS_IP 7777
```

If it connects and just sits there silently, the relay is up. Hit `Ctrl+C` to
get out.

If you ever want to change the port, add `Environment=XNET_PORT=9000` under the
`[Service]` section of `xnet.service`, then `daemon-reload` and restart. Open the
new port too, and set the matching port in the Xbox config.

---

## Part 2: Set Up the NXDK Build Environment

NXDK is the open source toolchain that compiles code for the original Xbox. Do
this on your Linux build machine.

### 2.1  Install the build dependencies

```bash
sudo apt update
sudo apt install -y \
    git cmake make \
    clang lld llvm \
    nasm \
    curl python3 \
    build-essential libc6-dev
```

### 2.2  Pick your NXDK

Read this part once, because it trips people up. There are two flavors of NXDK
and which one you want depends on the features you need:

- **Text chat and file transfer only:** mainline NXDK is fine.
- **Voice and video:** you need the **Ryzee119 NXDK fork** on the `xblc` branch.
  It includes the USB host driver that the Communicator headset and the EyeToy
  camera need. Mainline NXDK doesn't have it.

Most people want voice and video, so the fork is the usual pick. Choose one:

**Option A, full features (voice and video), Ryzee119 fork:**

```bash
git clone --recursive --branch xblc https://github.com/Ryzee119/nxdk ~/nxdk
```

**Option B, text and files only, mainline NXDK:**

```bash
git clone --recursive https://github.com/XboxDev/nxdk ~/nxdk
```

### 2.3  Build NXDK (one time only)

NXDK has to compile its own internal libraries before you can use it:

```bash
cd ~/nxdk
make NXDK_ONLY=y
```

This takes a few minutes. If it stops with an error, it's almost always a
missing package from step 2.1. Read the error, install whatever it names, and
run `make` again.

### 2.4  Point your shell at NXDK

XNET's Makefile needs to know where NXDK lives. Set this in any terminal you
build from:

```bash
export NXDK_DIR=~/nxdk
```

To avoid typing it every time, drop it into your shell profile:

```bash
echo 'export NXDK_DIR=~/nxdk' >> ~/.bashrc
```

One warning: do **not** put `source ~/nxdk/bin/activate` in your `~/.bashrc`. It
can hang new terminals. Setting `NXDK_DIR` like above is all XNET actually needs.

---

## Part 3: Get the XNET Source

Clone the XNET repository (swap the URL for wherever the repo lives):

```bash
git clone https://github.com/YOUR_USERNAME/XNET.git ~/XNET
cd ~/XNET
```

The Xbox app source lives in `_src/`, and the relay lives in `_src_relay/`.

---

## Part 4: Point XNET at Your Relay

XNET can find its relay address two ways, and you only need one of them.

The easy way, with no rebuild, is a plain text `xnet.cfg` file that sits next to
the app on the Xbox (you'll make this in Part 6). It overrides everything, so you
can leave the source alone and just edit a text file whenever you like.

If you'd rather bake your relay in so a fresh install works out of the box, open
`_src/main.c` and find this line:

```c
#define XNET_RELAY_HOST     "YOUR.RELAY.IP.HERE"
```

Set it to your relay's IP or hostname:

```c
#define XNET_RELAY_HOST     "your.relay.ip"
```

Either way is fine. If both are set, the config file wins.

---

## Part 5: Build the XBE

### 5.1  Start with a text only build

Building without audio and video first lets you confirm the networking and
encryption work before you add USB into the mix. This build uses stubs and runs
on either NXDK:

```bash
cd _src
make XNET_NO_AUDIO=1
```

You'll watch clang compile each file, then a link step. When it's done you'll
have your Xbox executable at:

```
_src/bin/default.xbe
```

### 5.2  The full build, with voice and video

Once text chat works on hardware, build the full app. This one needs the
Ryzee119 fork from Part 2:

```bash
cd _src
make clean
make
```

The output is again `_src/bin/default.xbe`, just larger this time since it now
includes the audio and camera drivers.

### 5.3  Rebuilding after edits

Any time you change a `.c` or `.h` file:

```bash
make clean      # optional, but guarantees nothing stale is left over
make            # or: make XNET_NO_AUDIO=1
```

---

## Part 6: Put XNET on Your Xbox

### Option A: FTP (easiest)

1. Boot your Xbox into its dashboard (XBMC4Gamers, UnleashX, EvoX, and so on).
2. Find the Xbox's FTP address on the dashboard's network screen.
3. Connect with FileZilla or any FTP client. The default login is often
   `xbox` / `xbox`.
4. Make a folder like `E:\Apps\XNET\`.
5. Upload `_src/bin/default.xbe` into it and name it `default.xbe`. Use **binary**
   transfer mode, not ASCII.
6. Create a text file named `xnet.cfg` in the same folder (template below).
7. Launch XNET from your dashboard's app list.

Here's a starter `xnet.cfg`. It's plain text, editable over FTP anytime, no
rebuild needed:

```ini
relay=your.relay.ip
port=7777
mic_gate=50000
mic_gain=100
debug_log=0
```

`mic_gate` is voice sensitivity, where `0` means an open mic that always
transmits. `mic_gain` is input volume from 25 to 200 percent. `debug_log=1` turns
on a verbose trace for bug reports. All of these can also be changed in the app
under **Settings**.

### Option B: XISO (boot as a disc image)

```bash
cd _src
make xiso       # builds an ISO you can launch from your loader
```

### Option C: Test in xemu first, no Xbox required

[xemu](https://xemu.app) is a PC emulator for the original Xbox. Load
`default.xbe` and you can test the menus, room tokens, and text chat without
touching hardware. USB audio and video don't work in xemu, so save those for a
real console.

---

## Part 7: First Run

Go through these in order. Each one proves the step before it worked.

1. **Boot XNET.** You should land on the main menu. If you see it, rendering and
   input are good.
2. **CREATE ROOM.** XNET connects to your relay and shows an 8 character token.
   If the token shows up, the console reached the relay. If you get an error
   screen instead, check that the relay is running, that port 7777 is open, and
   that the relay address is right in `xnet.cfg` (or `main.c`).
3. **JOIN from a second console.** Pick JOIN ROOM, type the token on the on
   screen keyboard, and confirm. Both consoles should drop into the room.
4. **Send a message.** If it shows up on both screens, your full
   encrypt, relay, decrypt path is working.
5. **Voice (full build).** Plug an Xbox Communicator into a controller's
   expansion slot on each console before booting, then talk. You should hear each
   other.
6. **Video (full build).** Plug in an EyeToy and open **VIDEO CHAT**.

---

## Troubleshooting

**`nxdk/...h not found`, or `make: nxdk-cc: not found`**
NXDK isn't on the path for this terminal. Run `export NXDK_DIR=~/nxdk` and try
`make` again. Add it to `~/.bashrc` so it sticks.

**`usb/usb.h not found`, or other USB and communicator build errors**
You're on mainline NXDK, which doesn't have the USB host driver. Either build
text only with `make XNET_NO_AUDIO=1`, or switch to the Ryzee119 `xblc` fork from
Part 2 for the full build.

**Build acting weird, or stale results**
Run `make clean`, then `make`. That clears the old build pieces so nothing stale
hangs around.

**The XBE won't boot, or acts strange after FTP**
Make sure you transferred in binary mode, the file is named exactly
`default.xbe`, and `xnet.cfg` is sitting in the same folder.

**Console shows an error right after CREATE ROOM**
The relay isn't reachable. Confirm the service is running
(`systemctl status xnet`), the port is open, and the address in `xnet.cfg`
matches your relay.

---

## Quick Reference

| Task | Command |
| ---- | ------- |
| Point shell at NXDK | `export NXDK_DIR=~/nxdk` |
| Text only build | `make XNET_NO_AUDIO=1` |
| Full build (voice and video) | `make` |
| Clean build | `make clean` |
| Build a bootable ISO | `make xiso` |
| Relay status | `sudo systemctl status xnet` |
| Restart relay | `sudo systemctl restart xnet` |

---

## A Note on the Relay

The relay is a blind forwarder. It moves encrypted packets and stores nothing. It
never holds the encryption key and can't read any traffic. Please keep it that
way: don't add logging, and run it as an unprivileged user. The included
`xnet.service` already handles both. See [`SECURITY.md`](SECURITY.md) and
[`SECURITY_ARCHITECTURE.md`](SECURITY_ARCHITECTURE.md) for the full design and
operator notes.

---

*Dead Orbit Studios, Tsardev*
