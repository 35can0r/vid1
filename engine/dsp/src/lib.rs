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

unsafe extern "C" {
    fn timeline_fps(handle: *const EngineHandle) -> f64;
}

// Helper types for the ring buffer
pub type HeapProducer = Caching<Arc<SharedRb<ringbuf::storage::Heap<f32>>>, true, false>;
pub type HeapConsumer = Caching<Arc<SharedRb<ringbuf::storage::Heap<f32>>>, false, true>;

pub struct AudioEngine {
    pub ring_producer: Arc<Mutex<HeapProducer>>,

    pub sample_position: Arc<AtomicI64>,
    pub is_playing: Arc<AtomicBool>,
    pub is_shutting_down: Arc<AtomicBool>,
    pub is_flushing: Arc<AtomicBool>,
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

        let supported_configs_range = device.supported_output_configs()
            .expect("error while querying configs");

        // Try to prefer f32 and 48000Hz or 44100Hz as requested in the prompt
        let supported_config = supported_configs_range
            .filter(|c| c.sample_format() == cpal::SampleFormat::F32)
            .find(|c| c.min_sample_rate().0 <= 48000 && c.max_sample_rate().0 >= 48000)
            .map(|c| c.with_sample_rate(cpal::SampleRate(48000)))
            .or_else(|| {
                device.supported_output_configs().unwrap()
                    .filter(|c| c.sample_format() == cpal::SampleFormat::F32)
                    .find(|c| c.min_sample_rate().0 <= 44100 && c.max_sample_rate().0 >= 44100)
                    .map(|c| c.with_sample_rate(cpal::SampleRate(44100)))
            })
            .or_else(|| device.supported_output_configs().unwrap().next().map(|c| c.with_max_sample_rate()))
            .expect("no supported config?!");

        let mut config = supported_config.config();

        // Ensure stereo as requested
        if config.channels > 2 {
            config.channels = 2;
        }
        let sample_rate = config.sample_rate.0;

        let rb = HeapRb::new(sample_rate as usize * 2); // 1 second stereo buffer
        let (prod, cons) = rb.split();
        let producer = Arc::new(Mutex::new(prod));

        let mut cons: HeapConsumer = cons;

        let sample_position = Arc::new(AtomicI64::new(0));
        let is_playing = Arc::new(AtomicBool::new(false));
        let is_shutting_down = Arc::new(AtomicBool::new(false));
        let is_flushing = Arc::new(AtomicBool::new(false));
        let target_frame = Arc::new(AtomicI64::new(0));

        let sp = sample_position.clone();
        let flush_flag = is_flushing.clone();

        // Error callback
        let err_fn = |err| eprintln!("an error occurred on stream: {}", err);

        let channels = config.channels as usize;

        // Data callback
        let data_fn = move |data: &mut [f32], _: &cpal::OutputCallbackInfo| {
            if flush_flag.swap(false, Ordering::Relaxed) {
                while cons.try_pop().is_some() {}
            }

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
            is_shutting_down,
            is_flushing,
            target_frame,
            stream: Some(stream),
            mixer_handle: None,
            sample_rate,
            timeline_handle: timeline,
        }
    }

    pub fn play(&mut self, from_frame: i64) {
        self.target_frame.store(from_frame, Ordering::Relaxed);

        let fps = if self.timeline_handle.is_null() {
            30.0
        } else {
            unsafe { timeline_fps(self.timeline_handle) }
        };

        let sample_pos = (from_frame as f64 / fps * self.sample_rate as f64) as i64;
        self.sample_position.store(sample_pos, Ordering::Relaxed);
        self.is_playing.store(true, Ordering::Relaxed);

        if let Some(stream) = &self.stream {
            let _ = stream.play();
        }

        if self.mixer_handle.is_none() {
            let timeline_handle = self.timeline_handle as usize;
            let producer = self.ring_producer.clone();
            let sample_position = self.sample_position.clone();
            let is_playing = self.is_playing.clone();
            let is_shutting_down = self.is_shutting_down.clone();
            let sample_rate = self.sample_rate;

            self.mixer_handle = Some(std::thread::spawn(move || {
                crate::mixer::run_mixer_thread(
                    timeline_handle as *mut EngineHandle,
                    producer,
                    sample_position,
                    is_playing,
                    is_shutting_down,
                    sample_rate,
                    fps
                );
            }));
        }
    }

    pub fn pause(&mut self) {
        self.is_playing.store(false, Ordering::Relaxed);
        if let Some(stream) = &self.stream {
            let _ = stream.pause();
        }
    }

    pub fn stop(&mut self) {
        self.pause();
        self.sample_position.store(0, Ordering::Relaxed);
        self.is_flushing.store(true, Ordering::Relaxed);
    }

    pub fn current_frame(&self) -> i64 {
        let pos = self.sample_position.load(Ordering::Relaxed);
        let fps = if self.timeline_handle.is_null() {
            30.0
        } else {
            unsafe { timeline_fps(self.timeline_handle) }
        };
        (pos as f64 / self.sample_rate as f64 * fps) as i64
    }

    pub fn destroy(&mut self) {
        self.stop();
        self.is_shutting_down.store(true, Ordering::Relaxed);
        if let Some(handle) = self.mixer_handle.take() {
            let _ = handle.join();
        }
    }
}
