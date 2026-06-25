// Fills output with silence — called when ring buffer is empty
pub fn fill_silence(output: &mut [f32]) {
    for sample in output.iter_mut() {
        *sample = 0.0;
    }
}
