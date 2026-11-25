"""F3d bare-board smoke. Main firmware, both USB connections, no hardware attached.
Creates temporary mock instances, saves/reboots, runs actions and cleans them up.
--safe-mode requires CONFIG_CORE_BENCH_CRASH=y; normal builds reject BenchCrash.
"""
import argparse
import struct
import time
import serial
from protocol import UartCommProto, CORE_PROTOCOL_VERSION

class Client:
    def __init__(self, port):
        self.serial=serial.Serial(port,921600,timeout=.1)
        self.proto=UartCommProto(self.serial)
        self.seq=0
    def request(self,family,op,body=b"",accepted=(0,),msg_type=0x01):
        self.seq=(self.seq+1)&255
        self.proto.send_command(msg_type,bytes((CORE_PROTOCOL_VERSION,self.seq,family,op))+body)
        deadline=time.monotonic()+4
        while time.monotonic()<deadline:
            kind,p=self.proto.read_frame(timeout=max(.01,deadline-time.monotonic()))
            if kind==0x41 and len(p)>=6 and p[1]==self.seq:
                assert p[:4]==bytes((CORE_PROTOCOL_VERSION,self.seq,family,op)),p
                assert p[4] in accepted, (family,op,p.hex())
                return p[6:]
        raise TimeoutError("core response")
    def info(self):
        p=self.request(1,1)
        offset=1
        for _ in range(2):
            n=struct.unpack_from("<H",p,offset)[0];offset+=2+n
        hash_,capacity,limit,count,flags=struct.unpack_from("<IIHBB",p,offset)
        offset+=struct.calcsize("<IIHBB")
        loop_count=p[offset];offset+=1;loops=[]
        for _ in range(loop_count):
            id_=p[offset];name_len=struct.unpack_from("<H",p,offset+1)[0];offset+=3
            name=p[offset:offset+name_len].decode();offset+=name_len
            rate,loop_flags=struct.unpack_from("<IB",p,offset);offset+=5
            loops.append(dict(id=id_,name=name,rate_hz=rate,realtime=bool(loop_flags&1)))
        device_id=p[offset:offset+6].hex();offset+=6
        n=p[offset];offset+=1
        name=p[offset:offset+n].decode("utf-8");offset+=n
        link_trial_ms,=struct.unpack_from("<I",p,offset);offset+=4
        assert len(device_id)==12 and n<=16 and offset==len(p)
        return dict(hash=hash_,limit=limit,count=count,flags=flags,loops=loops,device_id=device_id,name=name,link_trial_ms=link_trial_ms)
    def connect(self):
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            try:return self.info()
            except (TimeoutError,AssertionError):time.sleep(.2)
        raise TimeoutError("board did not boot")
    def page(self,op,prefix=b""):
        result=[];cursor=0
        while True:
            p=self.request(3,op,prefix+struct.pack("<H",cursor))
            total,next_,count=struct.unpack_from("<HHB",p)
            assert count or next_==65535
            result.append(p[5:])
            if next_==65535:return total,result
            assert next_>cursor;cursor=next_
    def reboot(self):
        self.request(1,8);time.sleep(.7);return self.connect()
    def add(self,type_,label,loop=255):
        name=label.encode();p=self.request(2,2,bytes((type_,loop,len(name)))+name)
        return p[0]
    def action(self,type_,inst,id_,values=(0,0,0,0),blob=b""):
        return self.request(8,1,struct.pack("<BBH4IH",type_,inst,id_,*values,len(blob))+blob)


def main():
    ap=argparse.ArgumentParser();ap.add_argument("--port",required=True);ap.add_argument("--safe-mode",action="store_true")
    args=ap.parse_args();c=Client(args.port)
    try:
        original=c.connect();print("GET_INFO",original,flush=True)
        if not args.safe_mode:
            c.request(1,254,accepted=(1,)) # normal builds must reject crash injection
        total,_=c.page(1);assert total>=4
        for type_ in (0,112,113,114):c.page(2,bytes((type_,)))
        c.page(4);print("PASS discovery and complete descriptor paging",flush=True)
        assert not original["flags"]&1,"discard or apply existing pending edits before smoke"
        actuator=c.add(112,"f3b_motor");feedback=c.add(113,"f3b_encoder");imu=c.add(114,"f3b_imu")
        wiring=(112<<8)|actuator
        c.request(4,2,bytes((1,))+struct.pack("<BBHI",113,feedback,1,wiring),accepted=(8,))
        pending=c.request(2,1,b"\x01\x00\x00");assert struct.unpack_from("<H",pending)[0]==original["count"]+3
        c.request(1,5);info=c.reboot();assert info["count"]==original["count"]+3
        half=struct.unpack("<I",struct.pack("<f",.5))[0]
        c.action(112,actuator,768,(half,0,0,0));time.sleep(.15)
        out=c.action(112,actuator,768,(half,0,0,0));assert struct.unpack_from("<f",out)[0]==0
        out=c.action(113,feedback,768);assert struct.unpack_from("<f",out)[0]==0
        out=c.action(112,actuator,769,blob=b"echo");assert out[18:]==b"echo"
        c.action(114,imu,768);c.action(114,imu,769)
        print("PASS layout, persistence, loop/paused actions and mocks",flush=True)
        if args.safe_mode:
            for _ in range(3):
                c.request(1,254);time.sleep(.7);info=c.connect()
            assert info["flags"]&8,info
            assert info["count"]==0
            assert info["loops"]==[]
            c.request(1,9);time.sleep(.7);info=c.connect();assert not info["flags"]&8
            assert info["count"]==original["count"]+3
            print("PASS crash-loop safe mode and recovery",flush=True)
        for type_,inst in ((113,feedback),(112,actuator),(114,imu)):
            c.request(2,3,bytes((type_,inst)))
        c.request(1,5);info=c.reboot();assert info["count"]==original["count"]
        deadline=time.monotonic()+3
        while True:
            kind,p=c.proto.read_frame(timeout=max(.01,deadline-time.monotonic()))
            if kind==0x80:assert len(p)==19+5*p[17] and p[0]==CORE_PROTOCOL_VERSION;break
            assert time.monotonic()<deadline
        print("PASS cleanup and status stream",flush=True)
    finally:c.serial.close()
if __name__=="__main__":main()
