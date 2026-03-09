use std::f32::consts::PI;
use std::sync::{OnceLock, Mutex};

#[derive(Clone, Copy)]
enum SynthType {
    Sine,
    Square,
    Sawtooth,
    Triangle,
}

struct SinePlugin {
    sample_rate: f32,
    phase: f32,
    frequency: f32,
    gain: f32,
    synth_type: SynthType,
}

static PLUGIN: OnceLock<Mutex<SinePlugin>> = OnceLock::new();

#[unsafe(no_mangle)]
pub extern "C" fn wasp_initialize(sample_rate: i32, _max_block_size: i32) {
    PLUGIN.get_or_init(|| Mutex::new(SinePlugin {
        sample_rate: sample_rate as f32,
        phase: 0.0,
        frequency: 440.0,
        gain: 0.5,
        synth_type: SynthType::Sine,
    }));
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_process(
    _inputs: *mut f32,
    outputs: *mut f32,
    num_frames: i32,
    num_channels: i32,
) {
    if let Some(mutex) = PLUGIN.get() {
        let mut plugin = mutex.lock().unwrap();

        let phase_increment = 2.0 * PI * plugin.frequency / plugin.sample_rate;

        for frame in 0..num_frames as usize {

            let mut sample_pregain = 0.0;

            match plugin.synth_type {
                SynthType::Sine => { sample_pregain = plugin.phase.sin(); }
                SynthType::Square => {
                    sample_pregain = plugin.phase.sin();
                    println!("square");

                    if (sample_pregain > 0.0) {
                        sample_pregain = 1.0
                    } else if (sample_pregain < 0.0) {
                        sample_pregain = -1.0;
                    }
                }
                SynthType::Sawtooth => {}
                SynthType::Triangle => {}
            }

            let sample = sample_pregain * plugin.gain;
            plugin.phase += phase_increment;

            if plugin.phase > 2.0 * PI {
                plugin.phase -= 2.0 * PI;
            }

            for channel in 0..num_channels as usize {
                let index = frame * num_channels as usize + channel;
                unsafe { *outputs.add(index) = sample; }
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_set_parameter(id: i32, value: f32) {
    if let Some(mutex) = PLUGIN.get() {
        let mut plugin = mutex.lock().unwrap();
        match id {
            0 => plugin.frequency = value,
            1 => plugin.gain = value,
            2 => plugin.synth_type = match value as i32 {
                0 => SynthType::Sine,
                1 => SynthType::Square,
                2 => SynthType::Sawtooth,
                3 => SynthType::Triangle,
                _ => SynthType::Sine,
            },
            _ => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_midi_event(status: i32, data1: i32, data2: i32) {
    if let Some(mutex) = PLUGIN.get() {
        let mut plugin = mutex.lock().unwrap(); // TODO: surely this is a problem
        match (status as u8) & 0xF0 {
            0x90 if data2 > 0 => {
                plugin.frequency = 440.0 * 2.0_f32.powf((data1 as f32 - 69.0) / 12.0);
                plugin.gain = 0.5;
            }
            0x80 | 0x90 => {
                plugin.gain = 0.0;
            }
            _ => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_terminate() {
    if let Some(mutex) = PLUGIN.get() {
        let mut plugin = mutex.lock().unwrap();
        plugin.phase = 0.0;
        plugin.gain = 0.0;
    }
}