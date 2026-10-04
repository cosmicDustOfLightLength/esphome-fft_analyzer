import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.components import microphone

DEPENDENCIES = ["microphone"]

fft_analyzer_ns = cg.esphome_ns.namespace("fft_analyzer")

FFTAnalyzer = fft_analyzer_ns.class_(
    "FFTAnalyzer",
    cg.Component,
)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(FFTAnalyzer),

    cv.Required("microphone"):
        cv.use_id(microphone.Microphone),

    # FFT window size, in samples. Must be a power of two. Larger = finer
    # frequency resolution but slower updates and more CPU per frame.
    cv.Optional("fft_size", default=512):
        cv.one_of(256, 512, 1024, int=True),

    # Peak-hold + decay smoothing factor.
    # 0.0 = no smoothing (bars track the raw signal instantly)
    # 0.99 = very slow, smooth fall-off after a peak
    cv.Optional("decay", default=0.85):
        cv.float_range(min=0.0, max=0.99),

    # Window overlap mode:
    # false (default) = each FFT window starts from a fresh, empty buffer.
    # true             = consecutive windows share 50% of their samples,
    #                     roughly doubling the update rate at the cost of
    #                     computing the FFT about twice as often.
    cv.Optional("overlap", default=False):
        cv.boolean,

    # Noise gate: any band/bar whose raw magnitude is below this value is
    # clamped to 0 before smoothing, to suppress microphone self-noise
    # flicker on quiet bands. 0.0 (default) disables the gate. Watch the
    # DEBUG log in a quiet room to find a good value for your hardware.
    cv.Optional("noise_floor", default=0.0):
        cv.float_range(min=0.0),

    # Number of generic, log-spaced visualizer "bars" for an equalizer
    # display (0 = disabled, default). Not exposed as HA sensors - read
    # them from a display lambda via id(<fft_id>).get_bar(i).
    cv.Optional("bar_count", default=0):
        cv.int_range(min=0, max=128),

    # Frequency range spanned by the bar_count bars, distributed
    # logarithmically (more resolution at low frequencies).
    cv.Optional("bar_low_frequency", default="50Hz"):
        cv.frequency,

    cv.Optional("bar_high_frequency", default="18000Hz"):
        cv.frequency,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[cv.GenerateID()])

    mic = await cg.get_variable(config["microphone"])
    cg.add(var.set_microphone(mic))

    cg.add(var.set_fft_size(config["fft_size"]))
    cg.add(var.set_decay(config["decay"]))
    cg.add(var.set_overlap(config["overlap"]))
    cg.add(var.set_noise_floor(config["noise_floor"]))

    if config["bar_count"] > 0:
        cg.add(var.set_bar_count(config["bar_count"]))
        cg.add(
            var.set_bar_freq_range(
                config["bar_low_frequency"],
                config["bar_high_frequency"],
            )
        )

    await cg.register_component(var, config)
