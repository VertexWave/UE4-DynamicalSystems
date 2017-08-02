extern crate tokio_core;
extern crate tokio_timer;
extern crate pretty_env_logger;
extern crate futures;
extern crate chrono;
extern crate uuid;

use std::io;
use std::net::SocketAddr;
use std::thread;
use std::sync::{Arc, Mutex};
use std::collections::VecDeque;

use futures::{Future, Stream, Sink};
use futures::future::{ok};
use tokio_core::net::{UdpSocket, UdpCodec};
use tokio_core::reactor::Core;

use uuid::Uuid;

mod rnd;

pub struct LineCodec;

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

type SyncSink<T> = futures::sink::Wait<futures::sync::mpsc::UnboundedSender<T>>;
type SharedQueue<T> = std::sync::Arc<std::sync::Mutex<std::collections::VecDeque<T>>>;

pub struct Client {
    uuid: Uuid,
    sender: SyncSink<Vec<u8>>,
    task: Option<std::thread::JoinHandle<()>>,
    queue: SharedQueue<Vec<u8>>,
}

#[no_mangle]
pub fn rd_netclient_msg_push(client: *mut Client, bytes: *const u8, count: u32) {
    unsafe {
        let count : usize = count as usize;
        let mut msg = Vec::with_capacity(count);
        for i in 0..count {
            msg.push(*bytes.offset(i as isize));
        }
        (*client).sender.send(msg).unwrap();
    }
}

#[no_mangle]
pub fn rd_netclient_msg_pop(client: *mut Client, msg: *mut u8) -> u32 {
    unsafe {
        let mut data : Vec<u8> = Vec::new();
        {
            if let Ok(mut locked_queue) = (*client).queue.try_lock() {
                if let Some(m) = locked_queue.pop_front() {
                    data = m;
                }
            }
        }
        for (i, c) in data.iter().enumerate() {
            *msg.offset(i as isize) = *c as u8;
        }
        data.len() as u32
    }
}

#[no_mangle]
pub fn rd_netclient_uuid(client: *mut Client, uuid: *mut u8) {
    unsafe {
        let u = format!("{}\npedro", (*client).uuid);
        for (i, c) in u.chars().enumerate().take(36) {
            *uuid.offset(i as isize) = c as u8;
        }
        *uuid.offset(36) = 0;
    }
}

#[no_mangle]
pub fn rd_netclient_drop(client: *mut Client) {
    unsafe { Box::from_raw(client) };
}

#[no_mangle]
pub fn rd_netclient_open(addr: *const char) -> *mut Client {

    let (ffi_tx, ffi_rx) = futures::sync::mpsc::unbounded::<Vec<u8>>();

    let queue: VecDeque<Vec<u8>> = VecDeque::new();
    let queue = Arc::new(Mutex::new(queue));

    let mut client = Box::new(Client{
        uuid: Uuid::new_v4(),
        sender: ffi_tx.wait(),
        task: None,
        queue: Arc::clone(&queue)
    });

    let task = thread::spawn(move || {

        let mut core = Core::new().unwrap();
        let handle = core.handle();
        let server_addr: SocketAddr = "138.68.41.91:8080".parse().unwrap();
        let local_addr: SocketAddr = "192.168.1.126:0".parse().unwrap();
        let udp_socket = UdpSocket::bind(&local_addr, &handle).unwrap();
        let (tx, rx) = udp_socket.framed(LineCodec).split();

        let msg_tx = ffi_rx.fold(tx, |tx, msg| {
            tx.send((server_addr, msg))
            .map_err(|_| ())
        });

        let msg_rx = rx.fold(queue, |queue, (_, msg)| {
            {
                let mut locked_queue = queue.lock().unwrap();
                locked_queue.push_back(msg);
            }
            ok::<SharedQueue<Vec<u8>>, std::io::Error>(queue)
        })
        .map_err(|_| ());

        core.run(Future::join(msg_tx, msg_rx)).unwrap();
    });

    client.task = Some(task);

    Box::into_raw(client)
}

unsafe fn from_buf_raw<T>(ptr: *const T, elts: usize) -> Vec<T> {
    let mut dst = Vec::with_capacity(elts);
    dst.set_len(elts);
    std::ptr::copy(ptr, dst.as_mut_ptr(), elts);
    dst
}

#[cfg(test)]
mod tests {

    use super::*;

    #[test]
    fn it_works() {
        rd_netclient_new();
        thread::sleep(Duration::from_secs(10000));
    }
}