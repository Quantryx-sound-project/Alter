#include <JuceHeader.h> //– obsahuje všetko (GUI, audio, threading, networking).
#include "MainComponent.h" //→ hlavná vizualizácia
#include "ControllerWindow.h"
#include "AlterState.h" //→ globálny stav aplikácie

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (AlterState& s)
        : juce::DocumentWindow ("ALTER",
                                juce::Colours::black,
                                juce::DocumentWindow::allButtons),
          settings (s)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);

        // Passneme AlterState do MainComponent
        setContentOwned (new MainComponent (settings), true);

        // HUD: 100 % šírky, ~3 % výšky obrazovky (min. 24 px)
        const auto screen = juce::Desktop::getInstance().getDisplays().getMainDisplay().userArea;
        const int hudH = juce::jmax (24, (int) std::round (screen.getHeight() * 0.03));
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
        // 1) centrálna konfigurácia
        settings = std::make_unique<AlterState>();

        
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

            // Detached panels are ALWAYS on top (HIGHEST priority) - no change needed
            // They are set in PanelWindow constructor with setAlwaysOnTop(true)
        };

        // 4c) Audio Mode switching callback
        controllerWindow->onAudioModeChanged = [this] (int mode)
        {
            if (mainWindow)
            {
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->setAudioSourceMode (mode);
            }
        };
    }

    void shutdown() override
    {
        controllerWindow = nullptr;
        mainWindow = nullptr;
        settings = nullptr;
    }

private:
    std::unique_ptr<AlterState>       settings;
    std::unique_ptr<MainWindow>       mainWindow;
    std::unique_ptr<ControllerWindow> controllerWindow;
};

START_JUCE_APPLICATION (AlterApplication)
