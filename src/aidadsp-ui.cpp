/*
 * Aida-X DPF plugin
 * Copyright (C) 2023 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DistrhoPluginCommon.hpp"
#include "DistrhoUI.hpp"
#include "DistrhoStandaloneUtils.hpp"

#include "Graphics.hpp"

#include "Layout.hpp"
#include "Widgets.hpp"

START_NAMESPACE_DISTRHO

// --------------------------------------------------------------------------------------------------------------------

enum ButtonIds {
    kButtonLoadModel = 1001,
    kButtonLoadCabinet,
    kButtonEnableMicInput
};

enum MicInputState {
    kMicInputUnsupported,
    kMicInputSupported,
    kMicInputEnabled,
    kMicInputJACK
};

class AidaDSPLoaderUI : public UI,
                        public ButtonEventHandler::Callback,
                        public KnobEventHandler::Callback
{
    float parameters[kNumParameters];

    struct {
        NanoImage aida;
        NanoImage ax;
        NanoImage background;
        NanoImage knob;
        NanoImage scale;
    } images;

    struct {
        ScopedPointer<AidaKnob> pregain;
        ScopedPointer<AidaKnob> bass;
        ScopedPointer<AidaKnob> middle;
        ScopedPointer<AidaKnob> treble;
        ScopedPointer<AidaKnob> depth;
        ScopedPointer<AidaKnob> presence;
        ScopedPointer<AidaKnob> master;
    } knobs;

    struct {
        ScopedPointer<AidaPluginSwitch> bypass;
        ScopedPointer<AidaPluginSwitch> eqpos;
        ScopedPointer<AidaPluginSwitch> midtype;
    } switches;

    struct {
        ScopedPointer<AidaSplitter> s1, s2, s3;
    } splitters;
    
    struct {
        ScopedPointer<AidaFileGroup> model;
        ScopedPointer<AidaFileGroup> cabsim;
    } loaders;

   #if DISTRHO_PLUGIN_VARIANT_STANDALONE
    ScopedPointer<AidaPushButton> micButton;
    MicInputState micInputState = kMicInputUnsupported;
   #endif

    HorizontalLayout subwidgetsLayout;

    enum {
        kFileLoaderNull,
        kFileLoaderModel,
        kFileLoaderImpulse,
    } fileLoaderMode = kFileLoaderNull;

public:
    /* constructor */
    AidaDSPLoaderUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT)
    {
        // Initialize parameters to their defaults
        for (uint i=0; i<kNumParameters; ++i)
            parameters[i] = kParameters[i].ranges.def;

        // Load resources
        using namespace Graphics;
        images.aida = createImageFromMemory(aidaData, aidaDataSize, IMAGE_GENERATE_MIPMAPS);
        images.ax = createImageFromMemory(axData, axDataSize, IMAGE_GENERATE_MIPMAPS);
        images.background = createImageFromMemory(backgroundData, backgroundDataSize, IMAGE_REPEAT_X|IMAGE_REPEAT_Y);
        images.knob = createImageFromMemory(knobData, knobDataSize, IMAGE_GENERATE_MIPMAPS);
        images.scale = createImageFromMemory(scaleData, scaleDataSize, IMAGE_GENERATE_MIPMAPS);
        // fontFaceId(createFontFromMemory("conthrax", conthrax_sbData, conthrax_sbDataSize, false));
        loadSharedResources();

        // Create subwidgets
        knobs.pregain = new AidaKnob(this, this, images.knob, images.scale, kParameterPREGAIN);
        knobs.bass = new AidaKnob(this, this, images.knob, images.scale, kParameterBASSGAIN);
        knobs.middle = new AidaKnob(this, this, images.knob, images.scale, kParameterMIDGAIN);
        knobs.treble = new AidaKnob(this, this, images.knob, images.scale, kParameterTREBLEGAIN);
        knobs.depth = new AidaKnob(this, this, images.knob, images.scale, kParameterDEPTH);
        knobs.presence = new AidaKnob(this, this, images.knob, images.scale, kParameterPRESENCE);
        knobs.master = new AidaKnob(this, this, images.knob, images.scale, kParameterMASTER);

        switches.bypass = new AidaPluginSwitch(this, this, kParameterGLOBALBYPASS);
        switches.eqpos = new AidaPluginSwitch(this, this, kParameterEQPOS);
        switches.midtype = new AidaPluginSwitch(this, this, kParameterMTYPE);

        splitters.s1 = new AidaSplitter(this);
        splitters.s2 = new AidaSplitter(this);
        splitters.s3 = new AidaSplitter(this);

        loaders.model = new AidaFileGroup(this, this, kParameterNETBYPASS, "MODEL", kButtonLoadModel, "Load Model...");
        loaders.model->setFilename("US-Double-Nrm-Model.json");

        loaders.cabsim = new AidaFileGroup(this, this, kParameterCABSIMBYPASS, "CABINET IR", kButtonLoadCabinet, "Load Cabinet IR...");
        loaders.cabsim->setFilename("US-Double-Nrm-Cab.wav");

       #if DISTRHO_PLUGIN_VARIANT_STANDALONE
        if (isUsingNativeAudio())
        {
            if (supportsAudioInput())
            {
                micButton = new AidaPushButton(this);
                micButton->setCallback(this);
                micButton->setId(kButtonEnableMicInput);
                micButton->setLabel("Enable Mic/Input");
                micInputState = kMicInputSupported;
            }
        }
        else
        {
            micInputState = kMicInputJACK;
        }
       #endif

        // Setup subwidgets layout
        subwidgetsLayout.widgets.push_back({ switches.bypass, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.pregain, Fixed });
        subwidgetsLayout.widgets.push_back({ splitters.s1, Fixed });
        subwidgetsLayout.widgets.push_back({ switches.eqpos, Fixed });
        subwidgetsLayout.widgets.push_back({ switches.midtype, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.bass, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.middle, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.treble, Fixed });
        subwidgetsLayout.widgets.push_back({ splitters.s2, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.depth, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.presence, Fixed });
        subwidgetsLayout.widgets.push_back({ splitters.s3, Fixed });
        subwidgetsLayout.widgets.push_back({ knobs.master, Fixed });
        repositionWidgets();

        // adjust size
        const double scaleFactor = getScaleFactor();
        setGeometryConstraints(DISTRHO_UI_DEFAULT_WIDTH * scaleFactor,
                               DISTRHO_UI_DEFAULT_HEIGHT * scaleFactor);

        if (scaleFactor != 1.0)
            setSize(DISTRHO_UI_DEFAULT_WIDTH*scaleFactor, DISTRHO_UI_DEFAULT_HEIGHT*scaleFactor);
    }

protected:
   /* -----------------------------------------------------------------------------------------------------------------
    * DSP/Plugin Callbacks */

   /**
      A parameter has changed on the plugin side.
      This is called by the host to inform the UI about parameter changes.
    */
    void parameterChanged(const uint32_t index, const float value) override
    {
        parameters[index] = value;

        switch (static_cast<Parameters>(index))
        {
        case kParameterINLPF:
            // TODO
            break;
        case kParameterPREGAIN:
            knobs.pregain->setValue(value, false);
            break;
        case kParameterNETBYPASS:
            loaders.model->setChecked(value < 0.5f);
            break;
        case kParameterEQBYPASS:
            // TODO
            break;
        case kParameterEQPOS:
            switches.eqpos->setChecked(value > 0.5f, false);
            break;
        case kParameterBASSGAIN:
            knobs.bass->setValue(value, false);
            break;
        case kParameterMIDGAIN:
            knobs.middle->setValue(value, false);
            break;
        case kParameterMTYPE:
            switches.midtype->setChecked(value > 0.5f, false);
            break;
        case kParameterTREBLEGAIN:
            knobs.treble->setValue(value, false);
            break;
        case kParameterDEPTH:
            knobs.depth->setValue(value, false);
            break;
        case kParameterPRESENCE:
            knobs.presence->setValue(value, false);
            break;
        case kParameterMASTER:
            knobs.master->setValue(value, false);
            break;
        case kParameterCABSIMBYPASS:
            loaders.cabsim->setChecked(value < 0.5f);
            break;
        case kParameterGLOBALBYPASS:
            switches.bypass->setChecked(value < 0.5f, false);
            break;
        case kParameterReportModelType:
            break;
        case kParameterReportCabinetLength:
            break;
        case kParameterBASSFREQ:
        case kParameterMIDFREQ:
        case kParameterMIDQ:
        case kParameterTREBLEFREQ:
        case kParameterCount:
            break;
        }
    }

    void stateChanged(const char* const key, const char* const value) override
    {
        if (std::strcmp(key, "json") == 0)
            return loaders.model->setFilename(value);
        if (std::strcmp(key, "ir") == 0)
            return loaders.cabsim->setFilename(value);
    }

   /* -----------------------------------------------------------------------------------------------------------------
    * Widget Callbacks */

    void onNanoDisplay() override
    {
        const uint width = getWidth();
        const uint height = getHeight();
        const double scaleFactor = getScaleFactor();

        const double cornerRadius = 12 * scaleFactor;
        const double marginHead = 12 * scaleFactor;
        const double widthPedal = kPedalWidth * scaleFactor;
        const double heightPedal = kPedalHeight * scaleFactor;
        const double widthHead = widthPedal - marginHead * 2;
        const double heightHead = 177 * scaleFactor;
        const double marginHorizontal = kPedalMargin * scaleFactor;
        const double marginVertical = kPedalMarginTop * scaleFactor;

        // outer bounds gradient
        beginPath();
        rect(0, 0, width, height);
        fillPaint(linearGradient(0, 0, 0, height,
                                 Color(0xcd, 0xff, 0x05).minus(50).invert(),
                                 Color(0x8b, 0xf7, 0x00).minus(50).invert()));
        fill();

        // outer bounds inner shadow matching host color, if provided
        Color bgColor;
        if (const uint hostBgColor = getBackgroundColor())
        {
            const int red   = (hostBgColor >> 24) & 0xff;
            const int green = (hostBgColor >> 16) & 0xff;
            const int blue  = (hostBgColor >>  8) & 0xff;
            bgColor = Color(red, green, blue);
        }
        else
        {
            bgColor = Color(0,0,0);
        }

        fillPaint(boxGradient(0, 0, width, height, cornerRadius, cornerRadius, bgColor.withAlpha(0.f), bgColor));
        fill();

        // box shadow
        beginPath();
        rect(marginHorizontal/2, marginVertical/2, marginHorizontal+widthPedal, marginVertical+heightPedal);
        fillPaint(boxGradient(marginHorizontal, marginVertical, widthPedal, heightPedal, cornerRadius, cornerRadius, Color(0,0,0,1.f), Color(0,0,0,0.f)));
        fill();

        // .rt-neural .grid
        beginPath();
        roundedRect(marginHorizontal, marginVertical, widthPedal, heightPedal, cornerRadius);
        fillPaint(linearGradient(marginHorizontal,
                                 marginVertical,
                                 marginHorizontal + widthPedal,
                                 marginVertical + heightPedal,
                                 Color(28, 23, 12),
                                 Color(19, 19, 19)));
        fill();

        // extra
        strokeColor(Color(150, 150, 150, 0.25f));
        stroke();

        // .rt-neural .background_head
        const Size<uint> headBgSize(images.background.getSize() / 2 * scaleFactor);

        beginPath();
        roundedRect(marginHorizontal + marginHead,
                    marginVertical + marginHead,
                    widthHead,
                    heightHead,
                    cornerRadius);
        fillPaint(linearGradient(marginHorizontal + marginHead,
                                 marginVertical + marginHead,
                                 marginHorizontal + marginHead,
                                 marginVertical + heightHead,
                                 Color(0x8b, 0xf7, 0x00),
                                 Color(0xcd, 0xff, 0x05)));
        fill();

        fillPaint(imagePattern(marginHorizontal + marginHead,
                               marginVertical + marginHead,
                               headBgSize.getWidth(),
                               headBgSize.getHeight(),
                               0.f, images.background, 1.f));
        fill();

        fillPaint(boxGradient(marginHorizontal + marginHead,
                              marginVertical + marginHead,
                              widthHead,
                              heightHead,
                              cornerRadius,
                              12 * scaleFactor,
                              Color(0, 0, 0, 0.f), Color(0, 0, 0)));
        fill();

        // .rt-neural .brand
        const Size<uint> aidaLogoSize(111 * scaleFactor, 25 * scaleFactor);

        save();
        translate(marginHorizontal + marginHead * 2, marginVertical + headBgSize.getHeight());
        beginPath();
        rect(0, 0, aidaLogoSize.getWidth(), aidaLogoSize.getHeight());
        fillPaint(imagePattern(0, 0, aidaLogoSize.getWidth(), aidaLogoSize.getHeight(), 0.f, images.aida, 1.f));
        fill();
        restore();

        // .rt-neural .plate
        const Size<uint> axLogoSize(250 * scaleFactor, 96 * scaleFactor);

        save();
        translate(marginHorizontal + widthPedal/2 - axLogoSize.getWidth()/2, marginVertical + marginHead + headBgSize.getHeight() / 6);
        beginPath();
        rect(0, 0, axLogoSize.getWidth(), axLogoSize.getHeight());
        fillPaint(imagePattern(0, 0, axLogoSize.getWidth(), axLogoSize.getHeight(), 0.f, images.ax, 1.f));
        fill();
        restore();

        fillColor(Color(0x0c, 0x2f, 0x03, 0.686f));
        fontSize(24 * scaleFactor);
        textAlign(ALIGN_CENTER | ALIGN_BASELINE);
        text(marginHorizontal + widthPedal/2, marginVertical + heightHead - marginHead, "AI CRAFTED TONE", nullptr);

       #if DISTRHO_PLUGIN_VARIANT_STANDALONE
        fillColor(Color(1.f,1.f,1.f));
        fontSize((kSubWidgetsFontSize + 1) * scaleFactor);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);

        const double micx = marginHorizontal + (micButton != nullptr ? 150 : 10) * scaleFactor;
        switch (micInputState)
        {
        case kMicInputUnsupported:
            text(micx, marginVertical/2, "Mic/Input Unsupported", nullptr);
            break;
        case kMicInputSupported:
            text(micx, marginVertical/2, "Please enable Mic/Input...", nullptr);
            break;
        case kMicInputEnabled:
            text(micx, marginVertical/2, "Mic/Input enabled", nullptr);
            break;
        case kMicInputJACK:
            text(micx, marginVertical/2, "Mic/Input always enabled (using JACK)", nullptr);
            break;
        }
       #endif
    }

    /*
    void onResize(const ResizeEvent& event) override
    {
        UI::onResize(event);
        repositionWidgets();
    }
    */

    void repositionWidgets()
    {
        const double scaleFactor = getScaleFactor();
        const double widthPedal = kPedalWidth * scaleFactor;
        const double heightPedal = kPedalHeight * scaleFactor;
        const double marginHorizontal = kPedalMargin * scaleFactor;
        const double marginTop = kPedalMarginTop * scaleFactor;
        const double margin = 15 * scaleFactor;

        const uint unusedSpace = widthPedal
                               - (AidaKnob::kScaleSize * scaleFactor * 7)
                               - (AidaPluginSwitch::kFullWidth  * scaleFactor * 3)
                               - (AidaSplitter::kLineWidth * scaleFactor * 3);
        const uint padding = unusedSpace / 14;
        const uint maxHeight = subwidgetsLayout.setAbsolutePos(marginHorizontal + margin,
                                                               marginTop + heightPedal - margin - kSubWidgetsFullHeight * scaleFactor,
                                                               padding);
        subwidgetsLayout.setSize(maxHeight, 0);

        const double loadersX = marginHorizontal + widthPedal * 2 / 3;

        loaders.model->setAbsolutePos(loadersX, marginTop + margin + margin / 2);
        loaders.model->setWidth(widthPedal / 3 - margin * 2);

        loaders.cabsim->setAbsolutePos(loadersX, marginTop + margin * 2 + loaders.model->getHeight());
        loaders.cabsim->setWidth(widthPedal / 3 - margin * 2);

       #if DISTRHO_PLUGIN_VARIANT_STANDALONE
        if (micButton != nullptr)
            micButton->setAbsolutePos(marginHorizontal, marginTop/2 - micButton->getHeight()/2);
       #endif
    }

    void buttonClicked(SubWidget* const widget, int button) override
    {
        if (button != kMouseButtonLeft)
            return;

        const uint id = widget->getId();

        switch (id)
        {
        case kParameterEQPOS:
        case kParameterMTYPE:
            editParameter(id, true);
            setParameterValue(id, static_cast<AidaPluginSwitch*>(widget)->isChecked() ? 1.f : 0.f);
            editParameter(id, false);
            break;
        case kParameterGLOBALBYPASS:
            editParameter(id, true);
            setParameterValue(id, static_cast<AidaPluginSwitch*>(widget)->isChecked() ? 0.f : 1.f);
            editParameter(id, false);
            break;
        case kButtonLoadModel:
            fileLoaderMode = kFileLoaderModel;
            requestStateFile("json", "model.json", "Open AidaDSP model json");
            break;
        case kButtonLoadCabinet:
            fileLoaderMode = kFileLoaderImpulse;
            requestStateFile("ir", "cab-ir.wav", "Open Cabinet Simulator IR");
            break;
        case kButtonEnableMicInput:
            if (supportsAudioInput() && !isAudioInputEnabled())
                requestAudioInput();
            break;
        }
    }

    void requestStateFile(const char* const stateKey, const char* const defaultName, const char* const title)
    {
       #ifndef DISTRHO_OS_WASM
        if (UI::requestStateFile(stateKey))
            return;
        d_stdout("File through host failed, doing it manually");
       #endif

        DISTRHO_NAMESPACE::FileBrowserOptions opts;
        opts.defaultName = defaultName;
        opts.title = title;

        if (!openFileBrowser(opts))
        {
            d_stdout("Failed to open a file dialog!");
        }
    }

    void knobDragStarted(SubWidget* const widget) override
    {
        editParameter(widget->getId(), true);
    }

    void knobDragFinished(SubWidget* const widget) override
    {
        editParameter(widget->getId(), false);
    }

    void knobValueChanged(SubWidget* const widget, float value) override
    {
        setParameterValue(widget->getId(), value);
    }

   #if DISTRHO_PLUGIN_VARIANT_STANDALONE
    void uiIdle() override
    {
        if (micButton != nullptr)
        {
            const MicInputState newMicInputState = isAudioInputEnabled() ? kMicInputEnabled : kMicInputSupported;

            if (micInputState != newMicInputState)
            {
                micInputState = newMicInputState;
                repaint();
            }
        }
    }
   #endif

   /**
      Window file selected function, called when a path is selected by the user, as triggered by openFileBrowser().
      This function is for plugin UIs to be able to override Window::onFileSelected(const char*).

      This action happens after the user confirms the action, so the file browser dialog will be closed at this point.
      The default implementation does nothing.
    */
    void uiFileBrowserSelected(const char* const filename) override
    {
        if (filename == nullptr)
            return;

        switch (fileLoaderMode)
        {
        case kFileLoaderNull:
            break;
        case kFileLoaderModel:
            setState("json", filename);
            break;
        case kFileLoaderImpulse:
            setState("ir", filename);
            break;
        }

        fileLoaderMode = kFileLoaderNull;
    }

    // ----------------------------------------------------------------------------------------------------------------

   /**
      Set our UI class as non-copyable and add a leak detector just in case.
    */
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AidaDSPLoaderUI)
};

/* --------------------------------------------------------------------------------------------------------------------
 * UI entry point, called by DPF to create a new UI instance. */

UI* createUI()
{
    return new AidaDSPLoaderUI();
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DISTRHO
