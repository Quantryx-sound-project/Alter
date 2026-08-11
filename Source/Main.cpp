#include <JuceHeader.h> //– obsahuje všetko (GUI, audio, threading, networking).
#include "MainComponent.h" //→ hlavná vizualizácia
#include "ControllerWindow.h"
#include "AlterState.h" //→ globálny stav aplikácie
#include "AlterTheme.h" //→ vizuálna identita (living architecture)
#include "LogoData.h"   //→ embedded brand logo SVG (app icon)
#include <cstring>
#include <map>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <timeapi.h>
 #pragma comment (lib, "winmm.lib")
#endif

// Render the brand logo into a square app/taskbar icon (white mark on transparent).
static juce::Image alterMakeAppIcon()
{
    juce::Image img (juce::Image::ARGB, 256, 256, true);
    if (auto d = juce::Drawable::createFromImageData (kAlterLogoSvg, std::strlen (kAlterLogoSvg)))
    {
        d->replaceColour (juce::Colours::black, juce::Colours::white);
        juce::Graphics g (img);
        d->drawWithin (g, img.getBounds().toFloat().reduced (22.0f),
                       juce::RectanglePlacement::centred, 1.0f);
    }
    return img;
}

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (AlterState& s)
        : juce::DocumentWindow ("ALTER",
                                juce::Colours::black,
                                juce::DocumentWindow::allButtons),
          settings (s)
    {
        // Borderless HUD: no native title bar (no minimize/maximize/close strip)
        setUsingNativeTitleBar (false);
        setTitleBarHeight (0);
        setResizable (true, true);

        // Passneme AlterState do MainComponent
        setContentOwned (new MainComponent (settings), true);

        // App / taskbar icon from the brand logo
        setIcon (alterMakeAppIcon());

        // HUD: 100 % šírky, ~18 % výšky obrazovky (väčšie okno pri štarte)
        const auto screen = juce::Desktop::getInstance().getDisplays().getMainDisplay().userArea;
        const int hudH = juce::jmax (1, (int) std::round (screen.getHeight() * 0.18));
        setBounds (screen.withY (screen.getY()).withX (screen.getX()));
        setSize (screen.getWidth(), hudH);
        
        auto panels = settings.getPanelsRoot();
        for (int i = 0; i < panels.getNumChildren(); ++i)
        {
            auto p = panels.getChild (i);

            if (! p.hasProperty (AlterState::kDetached))
                p.setProperty (AlterState::kDetached, false, nullptr);

            if (! p.hasProperty (AlterState::kWnd))
                p.setProperty (AlterState::kWnd, juce::Rectangle<int> (200,200,600,180).toString(), nullptr);
        }

        setVisible (true);

        // The HUD content (MainComponent) attaches its own shared OpenGL context
        // (AlterGLHost) for GPU-accelerated 2D + Synesthesia, so we must NOT force
        // the software renderer on this window any more.
    }

    void setShowControllerCallback (std::function<void()> cb)
    {
        if (auto* mc = dynamic_cast<MainComponent*>(getContentComponent()))
            mc->setShowControllerCallback (std::move (cb));
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplicationBase::quit();
    }

private:
    AlterState& settings;
};

class AlterApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "ALTER"; }
    const juce::String getApplicationVersion() override { return "0.2"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String&) override
    {
       #if JUCE_WINDOWS
        // ── Anti-stutter opt-out (the "choppy after ~30 s, mouse move fixes it") ──
        // After ~half a minute WITHOUT user input Windows starts coalescing timers
        // and power-throttling the process (EcoQoS): WaitableEvent/Timer waits
        // degrade from 1 ms to 15.6 ms granularity, which wrecks the 60 fps pacing
        // of the visual worker threads until the next mouse/keyboard input lifts
        // the throttle again. A real-time visualiser must opt out explicitly:
        timeBeginPeriod (1);                     // 1 ms timer resolution, whole process

       #ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        #define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
       #endif
        PROCESS_POWER_THROTTLING_STATE pt {};
        pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED
                       | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask   = 0;                      // 0 = never throttle, always honour the resolution
        SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof (pt));

        // ── The MESSAGE THREAD must be able to keep up with the workers ───────
        //
        // The two fixes above stop Windows from slowing the render workers down.
        // They do nothing about the other half of the problem: a frame that has
        // been rendered still has to be PRESENTED, and for most modules that goes
        // through this thread.
        //
        // Every module built on AsyncVisualBase finishes a frame on its worker and
        // then calls triggerAsyncUpdate(), which lands as a repaint() here. Only
        // then does JUCE re-cache the component and the GL context composite it.
        // The three that draw themselves with a shader are painted by the GL
        // thread and need nothing from this one — which is exactly the split the
        // symptom had. Some modules stuttered and some did not, and it was the
        // same some every time.
        //
        // The workers run at THREAD_PRIORITY_HIGHEST (juce::Thread::Priority::high
        // — see the note in AsyncVisualBase::startAsyncRender, they need it to stay
        // off efficiency cores). This thread was left at NORMAL, two levels below
        // several threads that never stop having work. Windows does rescue a
        // starved thread, but through the balance set manager, which sweeps about
        // once every four seconds — far too coarse to hold 60 fps together. In
        // between, repaints arrive late and unevenly.
        //
        // MOVING THE MOUSE HID IT. The input flood keeps this thread's scheduling
        // boost topped up, so it stops losing the race; stop moving it and the
        // boost decays. That is why it read as an idle problem, and why macOS never
        // showed it — no input-driven boost there and a different scheduler.
        //
        // Equal priority, not higher: among threads of the same priority Windows
        // round-robins, so this one is guaranteed to make progress without being
        // able to starve the workers back. Its per-frame work is a repaint mark,
        // measured in microseconds; the danger was never that it would run too
        // much, only that it would run too late.
        //
        // initialise() runs ON the message thread, so this is the right handle.
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
       #endif

        // 0) globálny vzhľad (ALTER theme)
        lookAndFeel = std::make_unique<AlterLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        // 1) centrálna konfigurácia
        settings = std::make_unique<AlterState>();

        // 1.0) aplikuj uloženú tému skôr než vzniknú okná
        AlterTheme::customPrimary   = juce::Colour ((uint32_t)(int) settings->getTree()
                                          .getProperty (AlterState::kThemeColor1, (int) 0xFF3D96E7));
        AlterTheme::customSecondary = juce::Colour ((uint32_t)(int) settings->getTree()
                                          .getProperty (AlterState::kThemeColor2, (int) 0xFF6902D6));
        AlterTheme::setTheme ((int) settings->getTree().getProperty (AlterState::kTheme, 0));
        lookAndFeel->refreshFromTheme();

        
        // 1.1) DEFAULTY – nastavíme ich len ak v strome ešte nie sú
        // ... v AlterApplication::initialise(...)
        auto& t = settings->getTree();
        if (! t.hasProperty ("rmsOn"))         settings->setRmsOn (true);
        if (! t.hasProperty ("rmsSmooth01"))   settings->setRmsSmooth01 (0.50f);   // stredná plynulosť

        if (! t.hasProperty ("spectrumOn"))    settings->setSpectrumOn (true);
        if (! t.hasProperty ("specAWeight"))   settings->setSpecAWeight (false);
        if (! t.hasProperty ("specSmooth01"))  settings->setSpecSmooth01 (0.70f);
        if (! t.hasProperty ("specScale"))     settings->setSpecScale ("log");     // len log používame
        if (! t.hasProperty ("specBins"))      settings->setSpecBins (1024);

        if (! t.hasProperty ("controllerAlwaysOnTop")) settings->setControllerAlwaysOnTop (false);
        if (! t.hasProperty ("systemGain"))  t.setProperty ("systemGain", 0.0f, nullptr);  // Default: 0 (true signal)

        // 2) okná
        mainWindow       = std::make_unique<MainWindow> (*settings);
        controllerWindow = std::make_unique<ControllerWindow> (*settings);

        // 3) klik na „CONTROLLER“ v HUD otvorí controller okno
        mainWindow->setShowControllerCallback ([this]
        {
            if (controllerWindow != nullptr)
            {
                controllerWindow->setVisible (true);
                controllerWindow->toFront (true);
            }
        });

        // 4) Z-ORDER HIERARCHY:
        // - Detached modules: HIGHEST (always on top in PanelWindow constructor)
        // - Controller: MIDDLE (always on top when enabled)
        // - HUD: LOWEST (never always on top by default, only when controller enables it)

        // Apply AlwaysOnTop for controller
        controllerWindow->setAlwaysOnTop (settings->controllerAlwaysOnTop());

        // 4b) ensure controller's AlwaysOnTop toggles HUD (but detached panels stay highest)
        controllerWindow->onAlwaysOnTopChanged = [this] (bool on)
        {
            // Controller window itself (MIDDLE priority)
            if (controllerWindow) controllerWindow->setAlwaysOnTop (on);

            // HUD main window (LOWEST priority when on)
            if (mainWindow) mainWindow->setAlwaysOnTop (on);

            // Detached module + block windows follow the checkbox too — with it
            // OFF they must be coverable by other applications.
            if (auto* mc = getMainComponent())
                mc->setAlwaysOnTopForAllDetachedPanels (on);
        };

        // 4c) Audio Mode switching callback
        controllerWindow->onAudioModeChanged = [this] (int mode)
        {
            if (auto* mc = getMainComponent())
                mc->setAudioSourceMode (mode);
        };

        // 4d) VST plugin instance picker plumbing
        controllerWindow->getPluginInstances = [this]() -> std::vector<PluginInstanceInfo>
        {
            if (auto* mc = getMainComponent())
                return mc->getUdpSource().getInstances();
            return {};
        };

        controllerWindow->onPluginInstanceSelected = [this] (juce::uint32 id)
        {
            if (auto* mc = getMainComponent())
                mc->getUdpSource().setSelectedInstance (id);
        };

        // Recording audio source: gap-free stereo stream of one plugin instance,
        // so an exported .mp4 can carry that plugin's track instead of the system mix.
        controllerWindow->pullPluginStereo = [this] (juce::uint32 id, std::uint64_t& ioTotal,
                                                     std::vector<float>& l, std::vector<float>& r) -> bool
        {
            if (auto* mc = getMainComponent())
                return mc->getUdpSource().getInstanceStreamStereo (id, ioTotal, l, r);
            return false;
        };

        controllerWindow->getPluginStreamRate = [this] (juce::uint32 id) -> double
        {
            if (auto* mc = getMainComponent())
                return mc->getUdpSource().getInstanceStreamRate (id);
            return 0.0;
        };

        // While recording from a plugin instance, that instance MUST keep sending
        // its waveform packets — the needs mask is otherwise built only from the
        // modules currently on screen, and without an oscilloscope or stereoscope
        // among them nobody asks for 'W', so the export ends up silent.
        controllerWindow->setRecordingAudioInstance = [this] (juce::uint32 id)
        {
            if (auto* mc = getMainComponent())
                mc->getUdpSource().setRecordingNeeds (id, id != 0 ? (juce::uint8) UdpReceiver::NeedWave
                                                                  : (juce::uint8) 0);
        };

        // Reshape the HUD to an aspect ratio. One shot: the window stays freely
        // resizable afterwards, this only puts it in the right shape to compose in.
        controllerWindow->setHudAspect = [this] (double ratio)
        {
            if (mainWindow == nullptr) return;

            const auto area = juce::Desktop::getInstance().getDisplays()
                                  .getDisplayForRect (mainWindow->getScreenBounds()) != nullptr
                                ? juce::Desktop::getInstance().getDisplays()
                                      .getDisplayForRect (mainWindow->getScreenBounds())->userArea
                                : juce::Desktop::getInstance().getDisplays()
                                      .getMainDisplay().userArea;

            if (ratio <= 0.0)
            {
                // The startup shape: full screen width, short banner across the top.
                const int h = juce::jmax (1, (int) std::round (area.getHeight() * 0.18));
                mainWindow->setBounds (area.getX(), area.getY(), area.getWidth(), h);
                return;
            }

            // Fill the display as far as the ratio allows, with a margin so the
            // window edges stay grabbable, then centre it.
            const double avail = 0.92;
            const int maxW = (int) (area.getWidth()  * avail);
            const int maxH = (int) (area.getHeight() * avail);

            int w = maxW, h = (int) std::lround (w / ratio);
            if (h > maxH) { h = maxH; w = (int) std::lround (h * ratio); }

            mainWindow->setBounds (area.getCentreX() - w / 2,
                                   area.getCentreY() - h / 2, w, h);
        };

        // Transparent capture: the modules paint themselves onto nothing, exactly
        // as they already do when Fusion borrows them as a layer.
        controllerWindow->setAlphaCaptureMode = [this] (bool on)
        {
            if (auto* mc = getMainComponent())
                mc->setAlphaCaptureMode (on);
        };

        controllerWindow->grabAlphaFrame = [this] (int w, int h) -> juce::Image
        {
            if (auto* mc = getMainComponent())
                return mc->grabAlphaFrame (nullptr, w, h);
            return {};
        };

        controllerWindow->grabAlphaFrameFor = [this] (juce::Component* c, int w, int h) -> juce::Image
        {
            if (auto* mc = getMainComponent())
                return mc->grabAlphaFrame (c, w, h);
            return {};
        };

        controllerWindow->getExportModules = [this]() -> std::vector<AlterExportModule>
        {
            std::vector<AlterExportModule> out;
            if (auto* mc = getMainComponent())
                for (const auto& m : mc->getExportableModules())
                    out.push_back ({ m.view, m.name, m.audioInstance, m.screenArea });
            return out;
        };

        controllerWindow->getEffectiveAudioInstance = [this]() -> juce::uint32
        {
            if (auto* mc = getMainComponent())
                return mc->getUdpSource().effectiveInstance();
            return 0;
        };

        controllerWindow->setRecordingAudioInstances = [this] (const std::vector<juce::uint32>& ids)
        {
            if (auto* mc = getMainComponent())
            {
                std::map<juce::uint32, juce::uint8> m;
                for (auto id : ids)
                    if (id != 0)
                        m[id] = (juce::uint8) UdpReceiver::NeedWave;

                mc->getUdpSource().setRecordingNeeds (m);
            }
        };

        // 4e) HUD window provider for the recorder
        controllerWindow->getHudComponent = [this]() -> juce::Component*
        {
            return static_cast<juce::Component*> (getMainComponent());
        };
    }

    void shutdown() override
    {
        controllerWindow = nullptr;
        mainWindow = nullptr;
        settings = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;

       #if JUCE_WINDOWS
        timeEndPeriod (1);   // matches the timeBeginPeriod(1) in initialise()
       #endif
    }

private:
    MainComponent* getMainComponent() const
    {
        return mainWindow ? dynamic_cast<MainComponent*> (mainWindow->getContentComponent())
                          : nullptr;
    }

    std::unique_ptr<AlterLookAndFeel> lookAndFeel;
    std::unique_ptr<AlterState>       settings;
    std::unique_ptr<MainWindow>       mainWindow;
    std::unique_ptr<ControllerWindow> controllerWindow;
};

START_JUCE_APPLICATION (AlterApplication)
