#![allow(non_snake_case)]

extern crate tokio_core;
extern crate futures;
extern crate uuid;
#[macro_use]
extern crate serde_derive;
extern crate bincode;
pub extern crate mumblebot;

use bincode::{serialize, deserialize, Infinite};

use std::io;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::thread;
use std::sync::{Arc, Mutex};
use std::collections::VecDeque;
use std::hash::{Hash, Hasher};

use futures::{Future, Stream, Sink};
use futures::future::{ok, err};
use tokio_core::net::{UdpSocket, UdpCodec};
use tokio_core::reactor::Core;

use std::os::raw::c_char;

mod rnd;

pub struct LineCodec;

extern fn null_log(_log: *const std::os::raw::c_char) {
    ()
}

static mut C_LOG: extern fn(log: *const c_char) = null_log;

#[no_mangle]
pub extern fn rb_log_fn(log_fn: extern fn(log: *const c_char)) {
    unsafe { C_LOG = log_fn; }
}

fn log(log: std::string::String) {
    let c_string = std::ffi::CString::new(log).unwrap();
    unsafe { C_LOG(c_string.as_ptr()) };
}

#[no_mangle]
pub fn rb_uuid() -> u32 {
    let mut s = std::collections::hash_map::DefaultHasher::new();
    uuid::Uuid::new_v4().hash(&mut s);
    s.finish() as u32
}

impl UdpCodec for LineCodec {
    type In = (SocketAddr, Vec<u8>);
    type Out = (SocketAddr, Vec<u8>);

    fn decode(&mut self, addr: &SocketAddr, buf: &[u8]) -> io::Result<Self::In> {
        Ok((*addr, buf.to_vec()))
    }

    fn encode(&mut self, (addr, buf): Self::Out, into: &mut Vec<u8>) -> SocketAddr {
        into.extend(buf);
        addr
    }
}

type SharedQueue<T> = std::sync::Arc<std::sync::Mutex<std::collections::VecDeque<T>>>;

pub struct Client {
    sender_pubsub: futures::sink::Wait<futures::sync::mpsc::Sender<Vec<u8>>>,
    msg_queue: SharedQueue<Vec<u8>>,
    kill: futures::sink::Wait<futures::sync::mpsc::Sender<()>>,
}

#[no_mangle]
pub fn rd_netclient_msg_push(client: *mut Client, bytes: *const u8, count: u32) {
    unsafe {
        let msg = std::slice::from_raw_parts(bytes, count as usize);
        let msg = Vec::from(msg);
        if let Err(err) = (*client).sender_pubsub.send(msg) {
            log(format!("rd_netclient_msg_push: {}", err));
        }
    }
}

#[no_mangle]
pub fn rd_netclient_msg_pop(client: *mut Client) -> *mut Vec<u8> {
    unsafe {
        let mut data : Vec<u8> = Vec::new();
        {
            if let Ok(mut locked_queue) = (*client).msg_queue.try_lock() {
                if let Some(m) = locked_queue.pop_front() {
                    data = m;
                }
            }
        }
        let data = Box::new(data);
        Box::into_raw(data)
    }
}

#[no_mangle]
pub fn rd_netclient_msg_drop(msg: *mut Vec<u8>) {
    unsafe { Box::from_raw(msg) };
}

#[no_mangle]
pub fn rd_netclient_drop(client: *mut Client) {
    unsafe {
        let mut client = Box::from_raw(client);
        let res = client.kill.send(());
        log(format!("rd_netclient_drop: {:?}", res));
    };
}

#[no_mangle]
pub fn rd_netclient_open(local_addr: *const c_char, server_addr: *const c_char, mumble_addr: *const c_char) -> *mut Client {
    let local_addr = unsafe { std::ffi::CStr::from_ptr(local_addr).to_owned().into_string().unwrap() };
    let server_addr = unsafe { std::ffi::CStr::from_ptr(server_addr).to_owned().into_string().unwrap() };
    let mumble_addr = unsafe { std::ffi::CStr::from_ptr(mumble_addr).to_owned().into_string().unwrap() };
    netclient_open(local_addr, server_addr, mumble_addr)
}

pub fn netclient_open(local_addr: String, server_addr: String, mumble_addr: String) -> *mut Client {

    let local_addr: SocketAddr = local_addr.parse().unwrap_or(SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), 0));
    let server_addr: SocketAddr = server_addr.parse().unwrap_or(SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), 0));
    let mumble_addr: SocketAddr = mumble_addr.parse().unwrap_or(SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), 0));

    let (kill_tx, kill_rx) = futures::sync::mpsc::channel::<()>(0);

    let (ffi_tx, ffi_rx) = futures::sync::mpsc::channel::<Vec<u8>>(1000);

    let (vox_out_tx, vox_out_rx) = futures::sync::mpsc::channel::<Vec<u8>>(1000);
    let (vox_inp_tx, vox_inp_rx) = futures::sync::mpsc::channel::<(i32, Vec<u8>)>(1000);

    let msg_queue: VecDeque<Vec<u8>> = VecDeque::new();
    let msg_queue = Arc::new(Mutex::new(msg_queue));

    let client = Box::new(Client{
        sender_pubsub: ffi_tx.wait(),
        msg_queue: Arc::clone(&msg_queue),
        kill: kill_tx.wait(),
    });

    thread::spawn(move || {

        let mut core = Core::new().unwrap();
        let handle = core.handle();

        let (mumble_loop, _tcp_tx, udp_tx) = mumblebot::run(local_addr, mumble_addr, vox_inp_tx.clone(), &handle);

        let mumble_say = mumblebot::say(vox_out_rx, udp_tx.clone());

        let kill_sink = mumblebot::gst::sink_main(vox_out_tx.clone());
        let (kill_src, mumble_listen) = mumblebot::gst::src_main(vox_inp_rx);
        
        let udp_socket = UdpSocket::bind(&local_addr, &handle).unwrap();
        let (tx, rx) = udp_socket.framed(LineCodec).split();

        let msg_out_task = ffi_rx.fold(tx, |tx, msg| {
            tx.send((server_addr, msg))
            .map_err(|_| ())
        })
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::Other, "msg_out_task"));

        let msg_inp_task = rx.fold(msg_queue, |queue, (_, msg)| {
            {
                let mut locked_queue = queue.lock().unwrap();
                locked_queue.push_back(msg);
            }
            ok::<SharedQueue<Vec<u8>>, std::io::Error>(queue)
        })
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::Other, "msg_inp_task"));

        let kill_switch = kill_rx
        .fold((), |_a, _b| {
            log(format!("kill_switch"));
            kill_sink();
            kill_src();
            err::<(),()>(())
        })
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::Other, "kill_switch"));

        let msg_tasks = Future::join(msg_inp_task, msg_out_task);
        let mum_tasks = Future::join(mumble_say, mumble_listen);

        if let Err(err) = core.run(Future::join4(mum_tasks, msg_tasks, mumble_loop, kill_switch)) {
        // if let Err(err) = core.run(Future::join(mum_tasks, mumble_loop)) {
            log(format!("rd_netclient_open: {}", err));
        }

        log(format!("core end"));

    });

    Box::into_raw(client)
}

#[repr(C)]
#[derive(Serialize, Deserialize, PartialEq, Debug, Clone)]
pub struct Rigidbody {
    id: u32,
    px: f32, py: f32, pz: f32, pw: f32,
    lx: f32, ly: f32, lz: f32, lw: f32,
    rx: f32, ry: f32, rz: f32, rw: f32,
    ax: f32, ay: f32, az: f32, aw: f32,
}

#[repr(C)]
#[derive(Serialize, Deserialize, PartialEq, Debug, Clone)]
pub struct Avatar {
    id: u32,
    root_px: f32, root_py: f32, root_pz: f32, root_pw: f32,
    root_rx: f32, root_ry: f32, root_rz: f32, root_rw: f32,
    head_px: f32, head_py: f32, head_pz: f32, head_pw: f32,
    head_rx: f32, head_ry: f32, head_rz: f32, head_rw: f32,
    handL_px: f32, handL_py: f32, handL_pz: f32, handL_pw: f32,
    handL_rx: f32, handL_ry: f32, handL_rz: f32, handL_rw: f32,
    handR_px: f32, handR_py: f32, handR_pz: f32, handR_pw: f32,
    handR_rx: f32, handR_ry: f32, handR_rz: f32, handR_rw: f32,
    height: f32, floor: f32, GripL: f32, GripR: f32, 
}

#[no_mangle]
pub fn rd_netclient_push_avatar(client: *mut Client, avatar: *const Avatar) {
    unsafe {
        let mut msg = vec![2u8];
        let mut encoded: Vec<u8> = serialize(&(*avatar), Infinite).unwrap();
        msg.append(&mut encoded);
        (*client).sender_pubsub.send(msg);
    }
}

#[no_mangle]
pub fn rd_netclient_dec_avatar(bytes: *const u8, count: u32) -> *const Avatar {
    unsafe {
        let msg = std::slice::from_raw_parts(bytes, count as usize);
        let avatar: Avatar = deserialize(msg).unwrap();
        let avatar = Box::new(avatar);
        Box::into_raw(avatar)
    }
}

#[no_mangle]
pub fn rd_netclient_drop_avatar(avatar: *mut Avatar) {
    unsafe { Box::from_raw(avatar) };
}

#[no_mangle]
pub fn rd_netclient_push_rigidbody(client: *mut Client, rigidbody: *const Rigidbody) {
    unsafe {
        let mut msg = vec![3u8];
        let mut encoded: Vec<u8> = serialize(&(*rigidbody), Infinite).unwrap();
        msg.append(&mut encoded);
        (*client).sender_pubsub.send(msg);
    }
}

#[no_mangle]
pub fn rd_netclient_dec_rigidbody(bytes: *const u8, count: u32) -> *const Rigidbody {
    unsafe {
        let msg = std::slice::from_raw_parts(bytes, count as usize);
        let rigidbody: Rigidbody = deserialize(msg).unwrap();
        let rigidbody = Box::new(rigidbody);
        Box::into_raw(rigidbody)
    }
}

#[no_mangle]
pub fn rd_netclient_drop_rigidbody(rigidbody: *mut Rigidbody) {
    unsafe { Box::from_raw(rigidbody) };
}
