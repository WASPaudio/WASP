use std::f32::consts::PI;
use std::sync::{OnceLock, Mutex};

const MAX_VOICES: usize = 8;

const WASP_EVENT_MIDI: u32  = 0;
const WASP_EVENT_PARAM: u32 = 1;

const PARAM_WAVEFORM: u32 = 0;
const PARAM_ATTACK: u32   = 1;
const PARAM_DECAY: u32    = 2;
const PARAM_SUSTAIN: u32  = 3;
const PARAM_RELEASE: u32  = 4;

#[derive(Clone, Copy)]
enum SynthType {
    Sine,
    Square,
    Sawtooth,
    Triangle,
}

#[derive(Clone, Copy, PartialEq)]
enum EnvelopeStage {
    Idle,
    Attack,
    Decay,
    Sustain,
    Release,
}

#[derive(Clone, Copy)]
struct Envelope {
    stage:        EnvelopeStage,
    level:        f32,  // current amplitude 0..1
    attack_rate:  f32,  // level change per sample
    decay_rate:   f32,
    sustain:      f32,  // sustain level 0..1
    release_rate: f32,
}

impl Envelope {
    fn new() -> Self {
        Envelope {
            stage:        EnvelopeStage::Idle,
            level:        0.0,
            attack_rate:  0.0,
            decay_rate:   0.0,
            sustain:      0.7,
            release_rate: 0.0,
        }
    }

    fn note_on(&mut self, attack_secs: f32, decay_secs: f32,
               sustain: f32, sample_rate: f32) {
        self.stage        = EnvelopeStage::Attack;
        self.sustain      = sustain;
        self.attack_rate  = if attack_secs > 0.0 { 1.0 / (attack_secs * sample_rate) } else { 1.0 };
        self.decay_rate   = if decay_secs  > 0.0 { 1.0 / (decay_secs  * sample_rate) } else { 1.0 };
    }

    fn note_off(&mut self, release_secs: f32, sample_rate: f32) {
        if self.stage != EnvelopeStage::Idle {
            self.stage        = EnvelopeStage::Release;
            self.release_rate = if release_secs > 0.0 { 1.0 / (release_secs * sample_rate) } else { 1.0 };
        }
    }

    fn is_active(&self) -> bool {
        self.stage != EnvelopeStage::Idle
    }

    fn process(&mut self) -> f32 {
        match self.stage {
            EnvelopeStage::Idle => {
                self.level = 0.0;
            }
            EnvelopeStage::Attack => {
                self.level += self.attack_rate;
                if self.level >= 1.0 {
                    self.level = 1.0;
                    self.stage = EnvelopeStage::Decay;
                }
            }
            EnvelopeStage::Decay => {
                self.level -= self.decay_rate;
                if self.level <= self.sustain {
                    self.level = self.sustain;
                    self.stage = EnvelopeStage::Sustain;
                }
            }
            EnvelopeStage::Sustain => {
                self.level = self.sustain;
            }
            EnvelopeStage::Release => {
                self.level -= self.release_rate;
                if self.level <= 0.0 {
                    self.level = 0.0;
                    self.stage = EnvelopeStage::Idle;
                }
            }
        }
        self.level
    }
}

#[derive(Clone, Copy)]
struct Voice {
    active: bool,
    note: u8,
    phase: f32,
    frequency: f32,
    velocity_gain: f32,
    age: u32,
    envelope: Envelope,
}

impl Voice {
    fn new() -> Self {
        Voice {
            active: false,
            note: 0,
            phase: 0.0,
            frequency: 440.0,
            velocity_gain: 0.5,
            age: 0,
            envelope: Envelope::new(),
        }
    }

    fn note_on(&mut self, note: u8, velocity: u8, sample_rate: f32,
               attack: f32, decay: f32, sustain: f32) {
        self.active        = true;
        self.note          = note;
        self.phase         = 0.0;
        self.frequency     = 440.0 * 2.0_f32.powf((note as f32 - 69.0) / 12.0);
        self.velocity_gain = (velocity as f32 / 127.0) * 0.5;
        self.envelope.note_on(attack, decay, sustain, sample_rate);
    }

    fn note_off(&mut self, release: f32, sample_rate: f32) {
        self.envelope.note_off(release, sample_rate);
    }

    fn render(&mut self, synth_type: SynthType, sample_rate: f32) -> f32 {
        if !self.active {
            return 0.0;
        }

        let env = self.envelope.process();

        // deactivate voice once envelope is idle
        if !self.envelope.is_active() {
            self.active = false;
            return 0.0;
        }

        let phase_increment = 2.0 * PI * self.frequency / sample_rate;

        let sample_pregain = match synth_type {
            SynthType::Sine => self.phase.sin(),
            SynthType::Square => {
                let s = self.phase.sin();
                if s > 0.0 { 1.0 } else if s < 0.0 { -1.0 } else { 0.0 }
            }
            SynthType::Sawtooth => (self.phase / PI) - 1.0,
            SynthType::Triangle => {
                let saw = (self.phase / PI) - 1.0;
                2.0 * saw.abs() - 1.0
            }
        };

        self.phase += phase_increment;
        if self.phase > 2.0 * PI {
            self.phase -= 2.0 * PI;
        }

        sample_pregain * self.velocity_gain * env
    }
}

struct Plugin {
    sample_rate: f32,
    synth_type:  SynthType,
    voices:      [Voice; MAX_VOICES],
    age_counter: u32,
    // ADSR params shared across all voices
    attack:      f32,
    decay:       f32,
    sustain:     f32,
    release:     f32,
}

impl Plugin {
    fn new(sample_rate: f32) -> Self {
        Plugin {
            sample_rate,
            synth_type:  SynthType::Sine,
            voices:      [Voice::new(); MAX_VOICES],
            age_counter: 0,
            attack:      0.01,
            decay:       0.1,
            sustain:     0.7,
            release:     0.3,
        }
    }

    fn note_on(&mut self, note: u8, velocity: u8) {
        let sr      = self.sample_rate;
        let attack  = self.attack;
        let decay   = self.decay;
        let sustain = self.sustain;

        // retrigger if already playing
        for voice in self.voices.iter_mut() {
            if voice.active && voice.note == note {
                voice.note_on(note, velocity, sr, attack, decay, sustain);
                voice.age = self.age_counter;
                self.age_counter += 1;
                return;
            }
        }
        // free voice
        for voice in self.voices.iter_mut() {
            if !voice.active {
                voice.note_on(note, velocity, sr, attack, decay, sustain);
                voice.age = self.age_counter;
                self.age_counter += 1;
                return;
            }
        }
        // steal oldest
        let oldest = self.voices.iter_mut().min_by_key(|v| v.age).unwrap();
        oldest.note_on(note, velocity, sr, attack, decay, sustain);
        oldest.age = self.age_counter;
        self.age_counter += 1;
    }

    fn note_off(&mut self, note: u8) {
        let release = self.release;
        let sr      = self.sample_rate;
        for voice in self.voices.iter_mut() {
            if voice.active && voice.note == note {
                voice.note_off(release, sr);
            }
        }
    }

    fn set_parameter(&mut self, id: u32, value: f32) {
        match id {
            PARAM_WAVEFORM => {
                self.synth_type = match value as i32 {
                    0 => SynthType::Sine,
                    1 => SynthType::Square,
                    2 => SynthType::Sawtooth,
                    3 => SynthType::Triangle,
                    _ => SynthType::Sine,
                };
            }
            PARAM_ATTACK  => self.attack  = value.max(0.001),
            PARAM_DECAY   => self.decay   = value.max(0.001),
            PARAM_SUSTAIN => self.sustain = value.clamp(0.0, 1.0),
            PARAM_RELEASE => self.release = value.max(0.001),
            _ => {}
        }
    }
}

// ── Memory layout structs ─────────────────────────────────────────────────────

#[repr(C)]
struct WaspEvent {
    event_type:    u32,
    sample_offset: u32,
    param0:        u32,
    param1:        u32,
    param2:        u32,
    param3:        u32,
}

#[repr(C)]
struct WaspTransport {
    playing:        u32,
    bpm:            f32,
    beat:           f32,
    time_sig_num:   u32,
    time_sig_denom: u32,
}

#[repr(C)]
struct WaspProcessContext {
    inputs_offset:  u32,
    outputs_offset: u32,
    input_count:    u32,
    output_count:   u32,
    frames:         u32,
    sample_rate:    u32,
    events_offset:  u32,
    event_count:    u32,
    transport:      WaspTransport,
}

static PROCESS_BUFFER: OnceLock<u32> = OnceLock::new();

fn get_or_alloc_process_buffer() -> u32 {
    *PROCESS_BUFFER.get_or_init(|| {
        static mut CTX_BUFFER: WaspProcessContext = WaspProcessContext {
            inputs_offset:  0,
            outputs_offset: 0,
            input_count:    0,
            output_count:   0,
            frames:         0,
            sample_rate:    0,
            events_offset:  0,
            event_count:    0,
            transport: WaspTransport {
                playing:        0,
                bpm:            120.0,
                beat:           0.0,
                time_sig_num:   4,
                time_sig_denom: 4,
            },
        };
        unsafe { &raw const CTX_BUFFER as u32 }
    })
}

static PLUGIN: OnceLock<Mutex<Plugin>> = OnceLock::new();

#[unsafe(no_mangle)]
pub extern "C" fn wasp_initialize(sample_rate: i32, _max_block_size: i32) {
    PLUGIN.get_or_init(|| Mutex::new(Plugin::new(sample_rate as f32)));
    get_or_alloc_process_buffer();
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_get_process_buffer() -> u32 {
    get_or_alloc_process_buffer()
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_process(ctx_offset: u32) {
    let base = 0usize;

    let ctx = unsafe {
        &*((base + ctx_offset as usize) as *const WaspProcessContext)
    };

    if let Some(mutex) = PLUGIN.get() {
        let mut plugin = mutex.lock().unwrap();

        let synth_type  = plugin.synth_type;
        let sample_rate = plugin.sample_rate;

        // Process all events first
        for i in 0..ctx.event_count as usize {
            let event = unsafe {
                &*((base + ctx.events_offset as usize
                    + i * size_of::<WaspEvent>()) as *const WaspEvent)
            };
            match event.event_type {
                WASP_EVENT_MIDI => {
                    let status = event.param0 as u8;
                    let data1  = event.param1 as u8;
                    let data2  = event.param2 as u8;
                    match status & 0xF0 {
                        0x90 if data2 > 0 => plugin.note_on(data1, data2),
                        0x80 | 0x90       => plugin.note_off(data1),
                        _                 => {}
                    }
                }
                WASP_EVENT_PARAM => {
                    let id    = event.param0;
                    let value = f32::from_bits(event.param1);
                    plugin.set_parameter(id, value);
                }
                _ => {}
            }
        }

        let output_count = ctx.output_count as usize;
        let frames       = ctx.frames as usize;

        let mut output_channels: [*mut f32; 8] = [std::ptr::null_mut(); 8];
        for ch in 0..output_count.min(8) {
            let ch_offset_ptr = (base + ctx.outputs_offset as usize
                + ch * size_of::<u32>()) as *const u32;
            let ch_offset = unsafe { *ch_offset_ptr };
            output_channels[ch] = (base + ch_offset as usize) as *mut f32;
        }

        for ch in 0..output_count.min(8) {
            if !output_channels[ch].is_null() {
                unsafe {
                    std::ptr::write_bytes(output_channels[ch], 0, frames);
                }
            }
        }

        for frame in 0..frames {
            let mut sample = 0.0f32;
            for voice in plugin.voices.iter_mut() {
                sample += voice.render(synth_type, sample_rate);
            }
            let sample = (sample * 0.8).tanh();

            for ch in 0..output_count.min(8) {
                if !output_channels[ch].is_null() {
                    unsafe {
                        *output_channels[ch].add(frame) = sample;
                    }
                }
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn wasp_terminate() {
    if let Some(mutex) = PLUGIN.get() {
        let mut plugin = mutex.lock().unwrap();
        let release = plugin.release;
        let sr      = plugin.sample_rate;
        for voice in plugin.voices.iter_mut() {
            if voice.active {
                voice.note_off(release, sr);
            }
        }
    }
}