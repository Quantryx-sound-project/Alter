/*
    AlterLicenseDialog.h
    ------------------------------------------------------------------
    Hotove aktivacne okno. Header-only, staci pridat include.

        #include "AlterLicenseDialog.h"

        // niekde v menu / na tlacidle "Licencia":
        alter::LicenseDialog::show (licence);

    Vsetky texty su na jednom mieste (namespace text nizsie), aby sa dali
    prelozit alebo preformulovat bez hrabania sa v layoute.

    Zamerne NEPOUZIVA vlastny LookAndFeel — prevezme ten tvoj z AlterTheme.h.

    ZAVISLOSTI: juce_gui_basics (uz je v Alter.jucer)
*/

#pragma once

#include "AlterLicense.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace alter
{

namespace text
{
    inline constexpr const char* kTitle        = "Licencia Alter";
    inline constexpr const char* kKeyHint      = "Vloz licencny kluc z potvrdzovacieho e-mailu";
    inline constexpr const char* kActivate     = "Aktivovat";
    inline constexpr const char* kBuy          = "Kupit Alter";
    inline constexpr const char* kContinueDemo = "Pokracovat v Demo";
    inline constexpr const char* kClose        = "Zavriet";
    inline constexpr const char* kVerifyNow    = "Overit teraz";
    inline constexpr const char* kDeactivate   = "Deaktivovat toto zariadenie";
    inline constexpr const char* kPortal       = "Spravovat predplatne";
    inline constexpr const char* kWorking      = "Pracujem...";

    inline constexpr const char* kDeactivateConfirm =
        "Alter sa na tomto pocitaci vrati do rezimu Demo a uvolni sa jeden "
        "aktivacny slot.\n\nKluc si potom mozes aktivovat na inom zariadeni.";

    inline constexpr const char* kNotConfigured =
        "Tento build Altera nema nastavenu licencnu konfiguraciu "
        "(AlterLicenseConfig.h). Aktivacia nie je mozna.";
}

//==============================================================================

class LicenseDialog : public juce::Component,
                      private juce::Timer
{
public:
    explicit LicenseDialog (LicenseManager& lm)
        : licence (lm)
    {
        setSize (460, 340);

        // ---- nadpis + aktualny stav ----
        addAndMakeVisible (planLabel);
        planLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        planLabel.setJustificationType (juce::Justification::centredLeft);

        addAndMakeVisible (detailLabel);
        detailLabel.setFont (juce::FontOptions (13.0f));
        detailLabel.setJustificationType (juce::Justification::topLeft);

        // ---- zadanie kluca ----
        addAndMakeVisible (keyEditor);
        keyEditor.setTextToShowWhenEmpty (text::kKeyHint,
                                          juce::Colours::grey);
        keyEditor.setInputRestrictions (64, "0123456789abcdefABCDEF-");
        keyEditor.onReturnKey = [this] { doActivate(); };
        keyEditor.onTextChange = [this] { statusLabel.setText ({}, juce::dontSendNotification); };

        addAndMakeVisible (activateButton);
        activateButton.setButtonText (text::kActivate);
        activateButton.onClick = [this] { doActivate(); };

        // ---- stavovy riadok ----
        addAndMakeVisible (statusLabel);
        statusLabel.setFont (juce::FontOptions (13.0f));
        statusLabel.setJustificationType (juce::Justification::topLeft);

        // ---- akcie ----
        addAndMakeVisible (primaryActionButton);   // kontextove: Kupit / Deaktivovat / ...
        primaryActionButton.setVisible (false);

        addAndMakeVisible (buyButton);
        buyButton.setButtonText (text::kBuy);
        buyButton.onClick = [] { LicenseManager::openStore(); };

        addAndMakeVisible (verifyButton);
        verifyButton.setButtonText (text::kVerifyNow);
        verifyButton.onClick = [this] { doValidate(); };

        addAndMakeVisible (deactivateButton);
        deactivateButton.setButtonText (text::kDeactivate);
        deactivateButton.onClick = [this] { confirmDeactivate(); };

        addAndMakeVisible (portalButton);
        portalButton.setButtonText (text::kPortal);
        portalButton.onClick = [] { LicenseManager::openCustomerPortal(); };

        addAndMakeVisible (closeButton);
        closeButton.onClick = [this] { dismiss(); };

        refresh();
    }

    ~LicenseDialog() override { stopTimer(); }

    /** Otvori okno. Vlastni sa samo. */
    static void show (LicenseManager& lm, juce::Component* parent = nullptr)
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new LicenseDialog (lm));
        o.dialogTitle                  = text::kTitle;
        o.componentToCentreAround      = parent;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar            = true;
        o.resizable                    = false;
        o.dialogBackgroundColour =
            juce::LookAndFeel::getDefaultLookAndFeel()
                .findColour (juce::ResizableWindow::backgroundColourId);
        o.launchAsync();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));

        // jemna deliaca ciara nad akciami
        g.setColour (findColour (juce::Label::textColourId).withAlpha (0.15f));
        g.fillRect (16, getHeight() - 96, getWidth() - 32, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);

        planLabel  .setBounds (r.removeFromTop (28));
        detailLabel.setBounds (r.removeFromTop (54));
        r.removeFromTop (8);

        auto keyRow = r.removeFromTop (30);
        activateButton.setBounds (keyRow.removeFromRight (100));
        keyRow.removeFromRight (8);
        keyEditor.setBounds (keyRow);

        r.removeFromTop (10);
        statusLabel.setBounds (r.removeFromTop (56));

        auto bottom = getLocalBounds().reduced (16).removeFromBottom (68);

        auto row1 = bottom.removeFromTop (30);
        primaryActionButton.setBounds (row1.removeFromLeft (220));
        closeButton.setBounds (row1.removeFromRight (100));

        bottom.removeFromTop (8);
        auto row2 = bottom.removeFromTop (30);
        buyButton      .setBounds (row2.removeFromLeft (110));
        row2.removeFromLeft (6);
        verifyButton   .setBounds (row2.removeFromLeft (100));
        row2.removeFromLeft (6);
        portalButton   .setBounds (row2.removeFromLeft (150));
        deactivateButton.setBounds (row2);
    }

    std::function<void()> onDismiss;

private:
    //==========================================================================

    void setBusy (bool b)
    {
        busy = b;
        activateButton  .setEnabled (! b);
        verifyButton    .setEnabled (! b);
        deactivateButton.setEnabled (! b);
        keyEditor       .setEnabled (! b);

        if (b)
        {
            dots = 0;
            startTimer (350);
        }
        else
        {
            stopTimer();
        }
    }

    void timerCallback() override
    {
        dots = (dots + 1) % 4;
        statusLabel.setText (juce::String (text::kWorking).upToFirstOccurrenceOf (".", false, false)
                               + juce::String::repeatedString (".", dots),
                             juce::dontSendNotification);
    }

    void setStatus (const juce::String& s, juce::Colour c)
    {
        stopTimer();
        statusLabel.setColour (juce::Label::textColourId, c);
        statusLabel.setText (s, juce::dontSendNotification);
    }

    juce::Colour errColour() const  { return juce::Colour (0xffe05252); }
    juce::Colour okColour() const   { return juce::Colour (0xff3fb950); }
    juce::Colour dimColour() const  { return findColour (juce::Label::textColourId).withAlpha (0.7f); }

    //==========================================================================

    void refresh()
    {
        const auto& s = licence.state();
        const auto  t = licence.tier();

        planLabel.setText (juce::String ("Plan: ") + tierName (t), juce::dontSendNotification);

        juce::StringArray lines;

        if (! LicenseManager::isConfigured())
        {
            lines.add (text::kNotConfigured);
        }
        else if (s.activated)
        {
            if (s.customerEmail.isNotEmpty())
                lines.add (s.customerEmail);

            if (s.activationLimit > 0)
                lines.add ("Zariadenia: " + juce::String (s.activationUsage)
                             + " / " + juce::String (s.activationLimit));

            if (s.expiresAt.toMilliseconds() > 0)
                lines.add ("Plati do: " + s.expiresAt.toString (true, false));
            else
                lines.add ("Bez casoveho obmedzenia");

            const int d = s.daysSinceValidation();
            if (d > config::kRevalidateEveryDays)
                lines.add ("Overene pred " + juce::String (d) + " dnami");
        }
        else
        {
            lines.add ("Demo: max " + juce::String (limitsFor (Tier::Demo).maxModules)
                         + " moduly, export s vodoznakom.");
            lines.add ("Aktivuj kluc alebo pokracuj v Demo.");
        }

        detailLabel.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
        detailLabel.setColour (juce::Label::textColourId, dimColour());

        const bool activated = s.activated;
        keyEditor       .setVisible (! activated);
        activateButton  .setVisible (! activated);
        deactivateButton.setVisible (activated);
        verifyButton    .setVisible (activated);
        portalButton    .setVisible (activated && s.expiresAt.toMilliseconds() > 0);
        buyButton       .setVisible (! activated);
        closeButton.setButtonText (activated ? text::kClose : text::kContinueDemo);

        activateButton.setEnabled (LicenseManager::isConfigured());

        primaryActionButton.setVisible (false);
        resized();
    }

    //==========================================================================

    void doActivate()
    {
        if (busy) return;

        setBusy (true);
        setStatus (text::kWorking, dimColour());

        licence.activateAsync (keyEditor.getText(),
                               [this] (LicenseManager::Result r) { handle (r, true); });
    }

    void doValidate()
    {
        if (busy) return;

        setBusy (true);
        setStatus (text::kWorking, dimColour());

        licence.validateAsync ([this] (LicenseManager::Result r) { handle (r, false); });
    }

    void confirmDeactivate()
    {
        if (busy) return;

        juce::NativeMessageBox::showOkCancelBox (
            juce::MessageBoxIconType::QuestionIcon,
            text::kDeactivate,
            text::kDeactivateConfirm,
            this,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                if (result == 0)
                    return;

                setBusy (true);
                setStatus (text::kWorking, dimColour());
                licence.deactivateAsync ([this] (LicenseManager::Result r) { handle (r, false); });
            }));
    }

    //==========================================================================

    void handle (LicenseManager::Result r, bool wasActivation)
    {
        setBusy (false);

        if (r.ok)
        {
            keyEditor.clear();
            refresh();
            setStatus (wasActivation
                         ? juce::String ("Aktivovane. Plan: ") + tierName (r.state.tier)
                         : juce::String ("Licencia je platna."),
                       okColour());
            return;
        }

        setStatus (r.error, r.isTransient() ? dimColour() : errColour());
        refresh();

        // Kontextove tlacidlo: konkretna akcia namiesto slepej ulicky.
        using F = LicenseManager::Failure;

        if (r.suggestsDeactivate())
        {
            showPrimary ("Uvolnit slot na tomto pocitaci",
                         [this] { confirmDeactivate(); });
        }
        else if (r.failure == F::Expired)
        {
            showPrimary ("Obnovit predplatne",
                         [] { LicenseManager::openCustomerPortal(); });
        }
        else if (r.failure == F::Disabled || r.failure == F::WrongStore)
        {
            showPrimary (juce::String ("Napisat na ") + config::kSupportEmail, [] {
                juce::URL (juce::String ("mailto:") + config::kSupportEmail)
                    .launchInDefaultBrowser();
            });
        }
        else if (r.failure == F::Network)
        {
            showPrimary ("Skusit znova", [this] { doActivate(); });
        }
        else if (r.failure == F::UnknownVariant)
        {
            // Diagnostika pre teba, nie pre zakaznika — ale nech to vidno.
            DBG ("ALTER: neznamy variant " << r.unknownVariantId);
        }
    }

    void showPrimary (const juce::String& label, std::function<void()> action)
    {
        primaryActionButton.setButtonText (label);
        primaryActionButton.onClick = std::move (action);
        primaryActionButton.setVisible (true);
        resized();
    }

    void dismiss()
    {
        if (onDismiss)
            onDismiss();

        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    }

    //==========================================================================

    LicenseManager& licence;

    juce::Label      planLabel, detailLabel, statusLabel;
    juce::TextEditor keyEditor;
    juce::TextButton activateButton, buyButton, verifyButton,
                     deactivateButton, portalButton, closeButton,
                     primaryActionButton;

    bool busy = false;
    int  dots = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseDialog)
};

} // namespace alter
