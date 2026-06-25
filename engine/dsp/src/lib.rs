pub mod mixer;
pub mod silence;
pub mod tests;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use ringbuf::traits::{Split, Consumer, Producer as ProducerTrait, Observer};
use ringbuf::HeapRb;
use ringbuf::wrap::caching::Caching;
use ringbuf::SharedRb;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicI64, AtomicBool, Ordering};
use std::thread::JoinHandle;

// The EngineHandle is an opaque pointer in dsp, fully defined in core::ffi
pub enum EngineHandle {}

// Helper types for the ring buffer
pub type HeapProducer = Caching<Arc<SharedRb<ringbuf::storage::Heap<f32>>>, true, false>;
pub type HeapConsumer = Caching<Arc<SharedRb<ringbuf::storage::Heap<f32>>>, false, true>;

pub struct AudioEngine {
    pub ring_producer: Arc<Mutex<HeapProducer>>,

    pub sample_position: Arc<AtomicI64>,
    pub is_playing: Arc<AtomicBool>,
    pub target_frame: Arc<AtomicI64>,

    pub stream: Option<cpal::Stream>,
    pub mixer_handle: Option<JoinHandle<()>>,

    pub sample_rate: u32,
    pub timeline_handle: *mut EngineHandle,
}

impl AudioEngine {
    pub fn new(timeline: *mut EngineHandle) -> Self {
        let host = cpal::default_host();
        let device = host.default_output_device().expect("No output device available");

        let mut supported_configs_range = device.supported_output_configs()
            .expect("error while querying configs");

        let supported_config = supported_configs_range.next()
            .expect("no supported config?!")
            .with_max_sample_rate();

        let config = supported_config.config();
        let sample_rate = config.sample_rate.0;

        let rb = HeapRb::new(sample_rate as usize * 2); // 1 second stereo buffer
        let (prod, cons) = rb.split();
        let producer = Arc::new(Mutex::new(prod));

        let mut cons: HeapConsumer = cons;

        let sample_position = Arc::new(AtomicI64::new(0));
        let is_playing = Arc::new(AtomicBool::new(false));
        let target_frame = Arc::new(AtomicI64::new(0));

        let sp = sample_position.clone();

        // Error callback
        let err_fn = |err| eprintln!("an error occurred on stream: {}", err);

        let channels = config.channels as usize;

        // Data callback
        let data_fn = move |data: &mut [f32], _: &cpal::OutputCallbackInfo| {
            let mut samples_read = 0;
            for sample in data.iter_mut() {
                if let Some(s) = cons.try_pop() {
                    *sample = s;
                    samples_read += 1;
                } else {
                    *sample = 0.0;
                }
            }
            // Increment sample position for one channel only to represent frames
            sp.fetch_add((samples_read / channels) as i64, Ordering::Relaxed);
        };

        let stream = device.build_output_stream(&config, data_fn, err_fn, None).unwrap();

        Self {
            ring_producer: producer,
            sample_position,
            is_playing,
            target_frame,
            stream: Some(stream),
            mixer_handle: None,
            sample_rate,
            timeline_handle: timeline,
        }
    }
}
