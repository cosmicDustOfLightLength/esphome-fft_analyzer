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

    cv.Optional("fft_size", default=512):
        cv.one_of(256, 512, 1024, int=True),

    # Bar decay factor (peak-hold + decay).
    # 0.0 = no smoothing (falls instantly to the raw value)
    # 0.99 = very slow fall-off
    cv.Optional("decay", default=0.85):
        cv.float_range(min=0.0, max=0.99),

    # Number of generic equalizer visualizer "bars" (log-spaced).
    # Does not create HA sensors - read only from a lambda: id(fft).get_bar(i)
    cv.Optional("bar_count", default=0):
        cv.int_range(min=0, max=128),

    # Frequency range distributed logarithmically across bar_count bars.
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

    if config["bar_count"] > 0:
        cg.add(var.set_bar_count(config["bar_count"]))
        cg.add(
            var.set_bar_freq_range(
                config["bar_low_frequency"],
                config["bar_high_frequency"],
            )
        )

    await cg.register_component(var, config)
