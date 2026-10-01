// Offscreen README / manual screenshots (SpectralHoldScreenshots target, not part of build.sh).
//
// Builds the real processor + editor, feeds synthetic stereo chords through processBlock
// while pumping the message loop (so SpectrumDisplay's 30 Hz timer smooths and repaints
// exactly as in a host), and renders scenes with createComponentSnapshot. The editor sits on
// the (Xvfb) desktop only so that mouse input can be injected through its ComponentPeer --
// hover (info bar, brush cursor) and press-and-hold brush strokes go through JUCE's normal
// mouse dispatch. Capture never touches the X server, so the old xwd failures in
// agent-wiki/build-and-test.md don't apply.
//
// Usage: xvfb-run -a SpectralHoldScreenshots <outDir>      (default outDir: docs/images)

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "SpectrumDisplay.h"

namespace
{
    constexpr double kSr    = 48000.0;
    constexpr int    kBlock = 512;
    constexpr float  kShot  = 1.5f; // snapshot scale (editor is 860x580 logical)

    const juce::Colour kMark { 0xffffb000 }; // annotation amber

    //==============================================================================
    // Test signals: a few harmonic voices per chord; L/R voiced slightly differently so the
    // split-stereo display (L up / R down) shows two distinct halves.
    struct Source
    {
        std::vector<double> voices;
        double t = 0.0;
        juce::Random rng { 7 };

        void render (juce::AudioBuffer<float>& buf)
        {
            auto* l = buf.getWritePointer (0);
            auto* r = buf.getWritePointer (1);
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                double sl = 0.0, sr = 0.0;
                for (size_t v = 0; v < voices.size(); ++v)
                    for (int h = 1; h <= 5; ++h)
                    {
                        const double ph = juce::MathConstants<double>::twoPi * voices[v] * h * t;
                        const double a  = 0.15 / h;
                        sl += a * (v % 2 == 0 ? 1.0 : 0.6) * std::sin (ph);
                        sr += a * (v % 2 == 0 ? 0.6 : 1.0) * std::sin (ph * 1.0015);
                    }
                const float n = 0.004f * (rng.nextFloat() * 2.0f - 1.0f);
                l[i] = (float) sl + n;
                r[i] = (float) sr + n;
                t += 1.0 / kSr;
            }
        }
    };

    Source chordA { { 110.0, 164.81, 220.0, 277.18, 329.63, 440.0 } };   // A major spread
    Source chordB { { 146.83, 349.23, 587.33, 698.46, 880.0 } };          // D minor, higher

    //==============================================================================
    struct Rig
    {
        SpectralHoldProcessor proc;
        std::unique_ptr<juce::AudioProcessorEditor> ed;

        Rig()
        {
            proc.setEditorScale (1.0f);
            proc.setPlayConfigDetails (2, 2, kSr, kBlock);
            proc.prepareToPlay (kSr, kBlock);
        }

        void set (const char* id, float plain)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (plain));
        }

        void defaults()
        {
            for (auto* p : proc.getParameters())
                p->setValueNotifyingHost (p->getDefaultValue());
        }

        // Run audio + message loop for `seconds` of audio, one GUI tick per ~33 ms.
        void run (double seconds, Source* src)
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            const int blocksPerTick = (int) std::ceil (kSr / 30.0 / kBlock);
            const int ticks = juce::jmax (1, (int) (seconds * 30.0));
            for (int tick = 0; tick < ticks; ++tick)
            {
                for (int b = 0; b < blocksPerTick; ++b)
                {
                    if (src != nullptr) src->render (buf);
                    else                buf.clear();
                    proc.processBlock (buf, midi);
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil (33);
            }
        }

        // Empty the hold: Loss all the way up with nothing coming in (Feed 0).
        void clearHold()
        {
            defaults();
            set ("feed", 0.0f);
            set ("loss", 1.0f);
            run (2.0, nullptr);
            defaults();
        }

        // Record `src` at the current params for `seconds`, then freeze (Feed 0, Loss 0).
        void capture (Source& src, double seconds = 3.0, float ew = 0.0f)
        {
            set ("ewLocation", ew);
            set ("loss", 0.0f);
            set ("feed", 0.3f);
            run (seconds, &src);
            set ("feed", 0.0f);
            run (0.5, nullptr);
        }

        // (Re)open the editor on a tab. On the desktop so its peer can take mouse input.
        void open (int tab)
        {
            mouseAway();
            ed.reset();
            proc.setUiTab (tab);
            ed.reset (proc.createEditor());
            ed->addToDesktop (0);
            ed->setVisible (true);
            run (0.3, nullptr);
        }

        //==========================================================================
        // Component lookup (the editor's members are private; find them by type/text).
        template <typename T>
        void collect (juce::Component& c, std::vector<T*>& out)
        {
            for (auto* child : c.getChildren())
            {
                if (auto* t = dynamic_cast<T*> (child)) out.push_back (t);
                collect (*child, out);
            }
        }

        juce::Rectangle<int> area (juce::Component& c) { return ed->getLocalArea (&c, c.getLocalBounds()); }

        SpectrumDisplay& display()
        {
            std::vector<SpectrumDisplay*> v; collect (*ed, v);
            return *v.at (0);
        }

        // Visible rotary knob whose label reads `text`; nth = 0 for the topmost match.
        juce::Slider& knob (const juce::String& text, int nth = 0)
        {
            std::vector<juce::Label*> labels; collect (*ed, labels);
            std::vector<juce::Slider*> sliders; collect (*ed, sliders);
            std::vector<std::pair<int, juce::Slider*>> hits;
            for (auto* l : labels)
            {
                if (! l->isShowing() || l->getText() != text) continue;
                const auto la = area (*l);
                for (auto* s : sliders)
                {
                    const auto sa = area (*s);
                    if (s->isShowing() && s->getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag
                        && std::abs (sa.getCentreX() - la.getCentreX()) < 6 && sa.getY() >= la.getBottom() - 2
                        && sa.getY() - la.getBottom() < 20)
                        hits.push_back ({ la.getY(), s });
                }
            }
            std::sort (hits.begin(), hits.end(), [] (auto& a, auto& b) { return a.first < b.first; });
            jassert ((int) hits.size() > nth);
            return *hits.at ((size_t) nth).second;
        }

        juce::Component& button (const juce::String& text)
        {
            std::vector<juce::Button*> v; collect (*ed, v);
            for (auto* b : v) if (b->isShowing() && b->getButtonText() == text) return *b;
            jassertfalse; return *v.front();
        }

        juce::Rectangle<int> displayArea() { return area (display()).expanded (8).getIntersection (ed->getLocalBounds()); }

        //==========================================================================
        // Mouse injection through the peer (editor-local == peer-local coordinates).
        void mouse (juce::Point<float> p, bool down)
        {
            if (auto* peer = ed != nullptr ? ed->getPeer() : nullptr)
                peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, p,
                                        down ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)
                                             : juce::ModifierKeys(),
                                        juce::MouseInputSource::defaultPressure,
                                        juce::MouseInputSource::defaultOrientation,
                                        juce::Time::currentTimeMillis());
        }
        void hover (juce::Component& c)            { mouse (area (c).getCentre().toFloat(), false); run (0.2, nullptr); }
        void mouseAway()                           { if (ed != nullptr) { mouse ({ -100.0f, -100.0f }, false); run (0.1, nullptr); } }

        // Point on the display at frequency `hz`, strength `s` (-1 cut .. +1 boost).
        juce::Point<float> displayPoint (float hz, float s)
        {
            const auto d = area (display()).toFloat();
            const float t = std::log (hz / 20.0f) / std::log (20000.0f / 20.0f);
            const float h = d.getHeight() - 40.0f; // minus the location strip
            return { d.getX() + t * (d.getWidth() - 1.0f), d.getY() + h * 0.5f * (1.0f - s) };
        }

        //==========================================================================
        // Capture with optional annotations, drawn at snapshot scale.
        struct Mark { juce::Rectangle<int> r; juce::String text; bool ring; };

        void save (const juce::String& name, juce::Rectangle<int> crop, const std::vector<Mark>& marks = {})
        {
            run (0.4, nullptr); // let the display settle on the final state
            auto img = ed->createComponentSnapshot (crop, true, kShot);
            {
                juce::Graphics g (img);
                g.addTransform (juce::AffineTransform::translation ((float) -crop.getX(), (float) -crop.getY())
                                    .scaled (kShot));
                for (auto& m : marks)
                {
                    if (m.ring)
                    {
                        g.setColour (kMark);
                        g.drawRoundedRectangle (m.r.toFloat().expanded (5.0f), 10.0f, 2.5f);
                    }
                    if (m.text.isNotEmpty()) badge (g, m.ring ? m.r.getTopLeft().toFloat() + juce::Point<float> (-4.0f, -4.0f)
                                                               : m.r.getCentre().toFloat(), m.text);
                }
            }
            const auto f = outDir.getChildFile (name + ".png");
            f.deleteFile();
            juce::FileOutputStream os (f);
            const bool ok = os.openedOk() && juce::PNGImageFormat().writeImageToStream (img, os);
            std::printf ("%s %s\n", ok ? "wrote" : "FAILED", f.getFileName().toRawUTF8());
            allOk &= ok;
        }

        static void badge (juce::Graphics& g, juce::Point<float> c, const juce::String& text)
        {
            const float r = 11.0f;
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (c.x - r - 1.0f, c.y - r + 1.0f, 2 * r + 2.0f, 2 * r + 2.0f);
            g.setColour (kMark);
            g.fillEllipse (c.x - r, c.y - r, 2 * r, 2 * r);
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText (text, juce::Rectangle<float> (c.x - r, c.y - r, 2 * r, 2 * r), juce::Justification::centred);
        }

        Mark ring (juce::Component& c, const juce::String& t = {}) { return { area (c), t, true }; }

        juce::File outDir;
        bool allOk = true;
    };
}

int main (int argc, char** argv)
{
    // JUCE only installs its (ignore-everything) X error handler for "standalone apps", i.e.
    // when createInstance is set. Without it, the first harmless BadAtom from a WM-less Xvfb
    // (no _NET_WM_* atoms) kills the process in Xlib's default handler.
    juce::JUCEApplicationBase::createInstance = [] () -> juce::JUCEApplicationBase* { return nullptr; };
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    rig.outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/images");
    rig.outDir.createDirectory();

    auto full = [&] { return rig.ed->getLocalBounds(); };
    auto disp = [&] { return rig.displayArea(); };
    // display + main knob row (for the tutorial: shows the hold and the knobs being turned)
    auto upper = [&] { return disp().getUnion (rig.area (rig.knob ("Release"))).withX (0).withRight (860).expanded (0, 6)
                                .getIntersection (full()); };

    //==== Getting started (tutorial) ===========================================
    rig.open (0);
    rig.defaults();
    rig.set ("feed", 0.0f);
    rig.set ("loss", 1.0f);
    rig.run (2.0, &chordA);
    rig.save ("tutorial-1-clear", upper(), { rig.ring (rig.knob ("Feed"), "1"), rig.ring (rig.knob ("Loss"), "2") });

    rig.set ("loss", 0.0f);
    rig.set ("feed", 0.3f);
    rig.run (2.5, &chordA);
    rig.save ("tutorial-2-capture", upper(), { rig.ring (rig.knob ("Loss"), "3"), rig.ring (rig.knob ("Feed"), "4") });

    rig.set ("feed", 0.0f);
    rig.set ("dryWet", 1.0f);
    rig.run (1.0, nullptr);
    rig.save ("tutorial-3-hold", upper(), { rig.ring (rig.knob ("Dry/Wet"), "5"), rig.ring (rig.knob ("Feed"), "6"),
                                            rig.ring (rig.knob ("Loss"), "7") });

    //==== Overview: annotated full editor (hovering Loss so the info bar shows) =====
    rig.hover (rig.knob ("Loss"));
    {
        std::vector<Rig::Mark> m;
        const auto d = rig.area (rig.display());
        m.push_back ({ { d.getX() + 18, d.getY() + 18, 0, 0 }, "A", false });
        m.push_back ({ { d.getX() + 18, d.getBottom() - 20, 0, 0 }, "B", false });
        const auto feed = rig.area (rig.knob ("Feed"));
        m.push_back ({ { 18, feed.getY() - 8, 0, 0 }, "C", false });
        const auto tab = rig.area (rig.button ("Shaper"));
        m.push_back ({ { 14, tab.getCentreY(), 0, 0 }, "D", false });
        const auto amt = rig.area (rig.knob ("Amount"));
        m.push_back ({ { 18, amt.getY() - 8, 0, 0 }, "E", false });
        const auto undo = rig.area (rig.button ("Undo"));
        m.push_back ({ { undo.getRight() + 18, undo.getCentreY(), 0, 0 }, "F", false });
        m.push_back ({ { 400, 570, 0, 0 }, "G", false });   // info bar (text from hovering Loss)
        m.push_back ({ { 860 - 34, 566, 0, 0 }, "H", false }); // resize corner
        rig.save ("overview", full(), m);
    }
    rig.mouseAway();

    //==== Brush ================================================================
    rig.mouse (rig.displayPoint (900.0f, 0.6f), false);
    rig.run (0.3, nullptr);
    rig.save ("brush-hover", disp());
    {
        const auto p = rig.displayPoint (330.0f, -0.8f);
        rig.mouse (p, false);
        rig.mouse (p, true);
        rig.run (2.0, nullptr);
        rig.save ("brush-cut", disp());
        rig.mouse (p, false);
    }
    rig.mouseAway();

    //==== East-West: two chords recorded at two spots ===========================
    rig.clearHold();
    rig.capture (chordA, 3.0, 0.0f);   // East
    rig.set ("loss", 0.0f);
    rig.set ("ewLocation", 1.0f);
    rig.set ("feed", 0.3f);
    rig.run (2.0, &chordB);            // West
    rig.set ("feed", 0.0f);
    rig.run (0.5, nullptr);
    rig.set ("ewLocation", 0.0f);
    rig.run (1.0, nullptr);
    rig.save ("ew-east", upper(), { rig.ring (rig.knob ("E<->W")) });
    rig.set ("ewLocation", 1.0f);
    rig.run (1.0, nullptr);
    rig.save ("ew-west", upper(), { rig.ring (rig.knob ("E<->W")) });

    //==== Shaper: each shape, momentary =========================================
    rig.clearHold();
    rig.capture (chordA);
    rig.open (0);
    struct ShapeShot { const char* name; float shape, freq, width, count, level; };
    const ShapeShot shapes[] = {
        { "shaper-level",     0.0f, 1000.0f, 1.0f, 0.5f,  0.8f },
        { "shaper-sigmoid",   1.0f, 500.0f,  0.5f, 1.0f,  0.7f },
        { "shaper-spikes",    2.0f, 440.0f,  0.4f, 1.0f, -0.7f },
        { "shaper-harmonics", 3.0f, 110.0f,  0.5f, 1.0f,  0.6f },
        { "shaper-sine",      4.0f, 600.0f,  0.4f, 1.0f, -0.7f },
    };
    for (auto& s : shapes)
    {
        rig.set ("shape", s.shape);       rig.set ("shapeFreq", s.freq);
        rig.set ("shapeWidth", s.width);  rig.set ("shapeCount", s.count);
        rig.set ("shapeLevel", s.level);
        rig.run (1.0, nullptr);
        rig.save (s.name, disp());
    }
    rig.save ("tab-shaper", full());
    // Permanent: same Harmonics cut written into the hold, then the shaper switched off
    rig.set ("shape", 3.0f); rig.set ("shapeFreq", 110.0f); rig.set ("shapeLevel", 0.6f);
    rig.set ("shapeMode", 1.0f);
    rig.run (3.0, nullptr);
    rig.set ("shapeLevel", 0.0f);
    rig.set ("shapeMode", 0.0f);
    rig.run (1.0, nullptr);
    rig.save ("shaper-permanent-after", disp());

    //==== Harmonize ==============================================================
    rig.clearHold();
    rig.capture (chordA);
    rig.open (1);
    rig.set ("harmonize", 0.05f);
    rig.set ("harmonic", 0.6f);
    rig.run (2.0, nullptr);
    rig.save ("tab-harmonize", full());

    //==== Alter ==================================================================
    rig.set ("harmonize", 0.0f);
    rig.clearHold();
    rig.capture (chordA);
    rig.open (2);
    rig.save ("alter-transpose-0", disp());
    rig.set ("transpose", 12.0f);
    rig.run (1.0, nullptr);
    rig.save ("alter-transpose-12", disp());
    rig.set ("transpose", 0.0f);
    rig.set ("spread", 1.0f);
    rig.run (1.0, nullptr);
    rig.save ("alter-spread", disp());
    rig.set ("spread", 0.0f);
    rig.set ("transpose", 5.0f);
    rig.set ("transposeSnap", 1.0f);
    rig.set ("spread", 0.6f);
    rig.set ("phaseNoiseAmt", 0.25f);
    rig.run (1.0, nullptr);
    rig.save ("tab-alter", full());

    //==== Reverb ================================================================
    rig.defaults();
    rig.set ("loss", 0.0f); rig.set ("feed", 0.0f);
    rig.open (3);
    rig.set ("revMix", 0.4f);
    rig.set ("revDecay", 0.8f);
    rig.set ("revFeed", 0.2f);
    rig.run (1.5, nullptr);
    rig.save ("tab-reverb", full());

    //==== FT size: the same chord at 1024 vs 8192 ===============================
    for (int order : { 10, 13 })
    {
        rig.defaults();
        rig.proc.setFftOrder (order); // resets the hold
        rig.open (0);
        rig.capture (chordA, 3.0);
        rig.save (juce::String ("ftsize-") + juce::String (1 << order), disp());
    }
    rig.proc.setFftOrder (12);

    rig.ed.reset();
    return rig.allOk ? 0 : 1;
}
