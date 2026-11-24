// Renders the plugin editor to a PNG without a host, for the README.
//   cmake -B build -DAD_COMPRESSOR_BUILD_SCREENSHOT_TOOL=ON && cmake --build build --target ADCompressorScreenshot
//   ./build/ADCompressorScreenshot_artefacts/Release/ADCompressorScreenshot docs/screenshot.png [seconds]
// [seconds]: first play that much voice-like test signal through the plugin,
// so the meters and the calibration status show a running state (README uses 40).
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <cstdio>

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;

    const juce::File out = juce::File::getCurrentWorkingDirectory()
                               .getChildFile (argc > 1 ? argv[1] : "docs/screenshot.png");

    ADCompressorProcessor processor;
    const double seconds = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 0.0;
    if (seconds > 0.0)
    {
        // Syllable-like bursts (harmonics on a gliding pitch) with pauses, at
        // about -20 dBFS, in 512-sample blocks like a host would send.
        const double sr = 48000.0;
        const int block = 512;
        processor.setRateAndBufferSizeDetails (sr, block);
        processor.prepareToPlay (sr, block);
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        juce::Random rng (7);
        double phase = 0, t = 0;
        float syllableLeft = 0, gapLeft = 0, amp = 0.1f;
        for (int b = 0; b < (int) (seconds * sr / block); ++b)
        {
            for (int i = 0; i < block; ++i, t += 1.0 / sr)
            {
                if (syllableLeft <= 0 && gapLeft <= 0)
                {
                    syllableLeft = 0.12f + 0.25f * rng.nextFloat();
                    gapLeft = rng.nextFloat() < 0.2f ? 0.4f + 0.8f * rng.nextFloat() : 0.03f;
                    amp = 0.06f + 0.08f * rng.nextFloat();
                }
                float v = 0;
                if (syllableLeft > 0)
                {
                    const double f0 = 120.0 + 30.0 * std::sin (t * 3.0);
                    phase += juce::MathConstants<double>::twoPi * f0 / sr;
                    for (int h = 1; h <= 12; ++h) v += (float) std::sin (phase * h) / (float) h;
                    v *= amp;
                    syllableLeft -= (float) (1.0 / sr);
                }
                else
                    gapLeft -= (float) (1.0 / sr);
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
            processor.processBlock (buf, midi);
        }
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setVisible (true);

    // Let the editor's update check finish (up to 5 s) so the shot shows a settled state.
    for (int i = 0; i < 50 && processor.updateChecker.getResult().status == UpdateChecker::Status::checking; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100); // one more UI refresh

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);

    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::fprintf (stderr, "Couldn't write %s\n", out.getFullPathName().toRawUTF8());
        return 1;
    }
    std::printf ("Wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}
