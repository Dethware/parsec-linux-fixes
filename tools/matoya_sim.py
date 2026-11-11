# Mimics libmatoya mty_evdev_poll: poll(timeout 0) then ONE 24-byte read per loop.
import os, select, struct, sys, time, threading
from evdev import UInput, ecodes as e, AbsInfo
caps={e.EV_KEY:[e.BTN_A,e.BTN_B], e.EV_ABS:[(c,AbsInfo(0,-32768,32767,16,128,0)) for c in (e.ABS_X,e.ABS_Y,e.ABS_RX,e.ABS_RY)]+[(e.ABS_HAT0X,AbsInfo(0,-1,1,0,0,0))]}
ui=UInput(caps,name="matoya-sim-src"); time.sleep(0.5)
fd=os.open(ui.device.path, os.O_RDWR|os.O_NONBLOCK)
FMT='llHHi'; SZ=struct.calcsize(FMT)
taps=40; stop=False
def writer():
    n=0
    for t in range(taps):
        for k in range(40):           # ~1 kHz trackpad/gyro style stick spam
            for c in (e.ABS_X,e.ABS_Y,e.ABS_RX,e.ABS_RY): ui.write(e.EV_ABS,c,(n*7919+c)%60000-30000)
            ui.syn(); n+=1; time.sleep(0.001)
        ui.write(e.EV_KEY,e.BTN_A,1); ui.syn(); time.sleep(0.03)
        ui.write(e.EV_KEY,e.BTN_A,0); ui.syn()
        ui.write(e.EV_ABS,e.ABS_HAT0X,1); ui.syn(); ui.write(e.EV_ABS,e.ABS_HAT0X,0); ui.syn()
threading.Thread(target=writer,daemon=True).start()
p=select.poll(); p.register(fd,select.POLLIN)
press=rel=hat=0; a_state=0; t0=time.time()
while time.time()-t0 < taps*0.08+2:
    if p.poll(0):
        try: b=os.read(fd,SZ)
        except BlockingIOError: b=b''
        if len(b)==SZ:
            _,_,typ,code,val=struct.unpack(FMT,b)
            if typ==e.EV_KEY and code==e.BTN_A:
                a_state=val; press+=val==1; rel+=val==0
            if typ==e.EV_ABS and code==e.ABS_HAT0X and val==1: hat+=1
    time.sleep(1/60)                   # Parsec app loop ~60 Hz
print(f"{os.environ.get('LD_PRELOAD','no shim')}: A presses {press}/{taps}, releases {rel}/{taps}, A stuck at end={a_state==1}, dpad taps {hat}/{taps}")
