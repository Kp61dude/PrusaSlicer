#include <iostream>
#include <utility>
#include <memory>

#include "GLScene.hpp"

#include <GL/glew.h>

#include <opencsg/opencsg.h>
// For compilers that support precompilation, includes "wx/wx.h".
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/slider.h>
#include <wx/tglbtn.h>
#include <wx/combobox.h>
#include <wx/spinctrl.h>
#include <wx/msgdlg.h>
#include <wx/glcanvas.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/SLAPrint.hpp"

#include "slic3r/GUI/Job.hpp"
#include "slic3r/GUI/ProgressStatusBar.hpp"

using namespace Slic3r::GL;

class Canvas: public wxGLCanvas, public Slic3r::GL::Display
{
    shptr<wxGLContext> m_context;
public:
    
    void set_active(long w, long h) override
    {
        SetCurrent(*m_context);
        Slic3r::GL::Display::set_active(w, h);
    }
    
    void swap_buffers() override { SwapBuffers(); }
    
    template<class...Args>
    Canvas(Args &&...args): wxGLCanvas(std::forward<Args>(args)...)
    {
        auto ctx = new wxGLContext(this);
        if (!ctx || !ctx->IsOK()) {
            wxMessageBox("Could not create OpenGL context.", "Error",
                         wxOK | wxICON_ERROR);
            return;
        }
        
        m_context.reset(ctx);
        
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            // This is required even though dc is not used otherwise.
            wxPaintDC dc(this);

            // Set the OpenGL viewport according to the client size of this
            // canvas. This is done here rather than in a wxSizeEvent handler
            // because our OpenGL rendering context (and thus viewport
            // setting) is used with multiple canvases: If we updated the
            // viewport in the wxSizeEvent handler, changing the size of one
            // canvas causes a viewport setting that is wrong when next
            // another canvas is repainted.
            const wxSize ClientSize = GetClientSize();
            
            set_screen_size(ClientSize.x, ClientSize.y);
//            repaint();
        });
    }

    ~Canvas() override
    {
        m_scene_cache.clear();
        m_context.reset();
    }
};

enum EEvents { LCLK_U, RCLK_U, LCLK_D, RCLK_D, DDCLK, SCRL, MV };
struct Event
{
    EEvents type;
    long    a, b;
    Event(EEvents t, long x = 0, long y = 0) : type{t}, a{x}, b{y} {}
};

class RecorderMouseInput: public MouseInput {
    std::vector<Event> m_events;
    bool m_recording = false, m_playing = false;
    
public:
    void left_click_down() override
    {
        if (m_recording) m_events.emplace_back(LCLK_D);
        if (!m_playing) MouseInput::left_click_down();
    }
    void left_click_up() override
    {
        if (m_recording) m_events.emplace_back(LCLK_U);
        if (!m_playing) MouseInput::left_click_up();
    }
    void right_click_down() override
    {
        if (m_recording) m_events.emplace_back(RCLK_D);
        if (!m_playing) MouseInput::right_click_down();
    }
    void right_click_up() override
    {
        if (m_recording) m_events.emplace_back(RCLK_U);
        if (!m_playing) MouseInput::right_click_up();
    }
    void double_click() override
    {
        if (m_recording) m_events.emplace_back(DDCLK);
        if (!m_playing) MouseInput::double_click();
    }
    void scroll(long v, long d, WheelAxis wa) override
    {
        if (m_recording) m_events.emplace_back(SCRL, v, d);
        if (!m_playing) MouseInput::scroll(v, d, wa);
    }
    void move_to(long x, long y) override
    {
        if (m_recording) m_events.emplace_back(MV, x, y);
        if (!m_playing) MouseInput::move_to(x, y);
    }
    
    void save(std::ostream &stream)
    {
        for (const Event &evt : m_events)
            stream << evt.type << " " << evt.a << " " << evt.b << std::endl;
    }
    
    void load(std::istream &stream)
    {
        m_events.clear();
        while (stream.good()) {
            int type; long a, b;
            stream >> type >> a >> b;
            m_events.emplace_back(EEvents(type), a, b);
        }
    }    
    
    void record(bool r) { m_recording = r; if (r) m_events.clear(); }
    
    void play()
    {
        m_playing = true;
        for (const Event &evt : m_events) {
            switch (evt.type) {
            case LCLK_U: MouseInput::left_click_up(); break;
            case LCLK_D: MouseInput::left_click_down(); break;
            case RCLK_U: MouseInput::right_click_up(); break;
            case RCLK_D: MouseInput::right_click_down(); break;
            case DDCLK:  MouseInput::double_click(); break;
            case SCRL:   MouseInput::scroll(evt.a, evt.b, WheelAxis::waVertical); break;
            case MV:     MouseInput::move_to(evt.a, evt.b); break;
            }
            
            wxSafeYield();
        }
        m_playing = false;
    }
};

class MyFrame: public wxFrame
{
    shptr<Scene>      m_scene;    // Model
    shptr<Canvas>     m_canvas;   // View
    shptr<Controller> m_ctl;      // Controller

    shptr<Slic3r::GUI::ProgressStatusBar> m_stbar;
    
    RecorderMouseInput m_mouse;
    
    class SLAJob: public Slic3r::GUI::Job {
        MyFrame *m_parent;
        std::unique_ptr<Slic3r::SLAPrint> m_print;
        std::string m_fname;
        
    public:
        SLAJob(MyFrame *frame, const std::string &fname)
            : Slic3r::GUI::Job{frame->m_stbar}
            , m_parent{frame}
            , m_fname{fname}
        {}

        void process() override;
        
        const std::string & get_project_fname() const { return m_fname; }
        
    protected:
        void finalize() override 
        {
            m_parent->m_scene->set_print(std::move(m_print));
            m_parent->m_stbar->set_status_text(
                        wxString::Format("Model %s loaded.", m_fname));
        }
    };
    
    uqptr<SLAJob> m_ui_job;
    
public:
    MyFrame(const wxString& title, const wxPoint& pos, const wxSize& size);
    
    void load_model(const std::string &fname) {
        m_ui_job = std::make_unique<SLAJob>(this, fname);
        m_ui_job->start();
    }
    
    void play_back_mouse(const std::string &events_fname)
    {
        std::fstream stream(events_fname, std::fstream::in);

        if (stream.good()) {
            std::string model_name;
            std::getline(stream, model_name);
            load_model(model_name);
            m_mouse.load(stream);
            m_mouse.play();
        }
    }
    
    void bind_canvas_events(MouseInput &msinput);
};

class App : public wxApp {
    MyFrame *m_frame;
    
public:
    bool OnInit() override {
        
        std::string fname;
        std::string command;
        
        if (argc > 2) {
            command = argv[1];
            fname = argv[2];
        }
            
        m_frame = new MyFrame("PrusaSlicer OpenCSG Demo", wxDefaultPosition, wxSize(1024, 768));
        
        if (command == "play") {
            m_frame->Show( true );
            m_frame->play_back_mouse(fname);
            m_frame->Close( true );
        } else m_frame->Show( true );
        
        return true;
    }
};

wxIMPLEMENT_APP(App);

MyFrame::MyFrame(const wxString &title, const wxPoint &pos, const wxSize &size):
    wxFrame(nullptr, wxID_ANY, title, pos, size)
{
    wxMenu *menuFile = new wxMenu;
    menuFile->Append(wxID_OPEN);
    menuFile->Append(wxID_EXIT);
    wxMenuBar *menuBar = new wxMenuBar;
    menuBar->Append( menuFile, "&File" );
    SetMenuBar( menuBar );
    
    m_stbar = std::make_shared<Slic3r::GUI::ProgressStatusBar>(this);
    m_stbar->embed(this);
    
    SetStatusText( "Welcome to wxWidgets!" );
    
    int attribList[] =
    {WX_GL_RGBA, WX_GL_DOUBLEBUFFER,
     // RGB channels each should be allocated with 8 bit depth. One
     // should almost certainly get these bit depths by default.
     WX_GL_MIN_RED, 8, WX_GL_MIN_GREEN, 8, WX_GL_MIN_BLUE, 8,
     // Requesting an 8 bit alpha channel. Interestingly, the NVIDIA
     // drivers would most likely work with some alpha plane, but
     // glReadPixels would not return the alpha channel on NVIDIA if
     // not requested when the GL context is created.
     WX_GL_MIN_ALPHA, 8, WX_GL_DEPTH_SIZE, 8, WX_GL_STENCIL_SIZE, 8,
     /*WX_GL_SAMPLE_BUFFERS, GL_TRUE, WX_GL_SAMPLES, 4,*/ 0};
    
    m_scene = std::make_shared<Scene>();
    m_ctl = std::make_shared<Controller>();
    m_ctl->set_scene(m_scene);
    
    m_canvas = std::make_shared<Canvas>(this, wxID_ANY, attribList,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxWANTS_CHARS | wxFULL_REPAINT_ON_RESIZE);
    m_ctl->add_display(m_canvas);

    wxPanel *control_panel = new wxPanel(this);

    auto controlsizer = new wxBoxSizer(wxHORIZONTAL);
    auto slider_sizer = new wxBoxSizer(wxVERTICAL);
    auto console_sizer = new wxBoxSizer(wxVERTICAL);
    
    auto slider = new wxSlider(control_panel, wxID_ANY, 0, 0, 100,
                               wxDefaultPosition, wxDefaultSize,
                               wxSL_VERTICAL);
    slider_sizer->Add(slider, 1, wxEXPAND);
    
    auto ms_toggle = new wxToggleButton(control_panel, wxID_ANY, "Multisampling");
    console_sizer->Add(ms_toggle, 0, wxALL | wxEXPAND, 5);
    
    auto csg_toggle = new wxToggleButton(control_panel, wxID_ANY, "CSG");
    csg_toggle->SetValue(true);
    console_sizer->Add(csg_toggle, 0, wxALL | wxEXPAND, 5);
    
    auto add_combobox = [control_panel, console_sizer]
            (const wxString &label, std::vector<wxString> &&list)
    {
        auto widget = new wxComboBox(control_panel, wxID_ANY, list[0],
                wxDefaultPosition, wxDefaultSize,
                int(list.size()), list.data());
        
        auto sz = new wxBoxSizer(wxHORIZONTAL);
        sz->Add(new wxStaticText(control_panel, wxID_ANY, label), 0,
                wxALL | wxALIGN_CENTER, 5);
        sz->Add(widget, 1, wxALL | wxEXPAND, 5);
        console_sizer->Add(sz, 0, wxEXPAND);
        return widget;
    };
    
    auto add_spinctl = [control_panel, console_sizer]
            (const wxString &label, int initial, int min, int max)
    {    
        auto widget = new wxSpinCtrl(
                    control_panel, wxID_ANY,
                    wxString::Format("%d", initial),
                    wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min, max,
                    initial);
        
        auto sz = new wxBoxSizer(wxHORIZONTAL);
        sz->Add(new wxStaticText(control_panel, wxID_ANY, label), 0,
                wxALL | wxALIGN_CENTER, 5);
        sz->Add(widget, 1, wxALL | wxEXPAND, 5);
        console_sizer->Add(sz, 0, wxEXPAND);
        return widget;
    };
    
    auto convexity_spin = add_spinctl("Convexity", CSGSettings::DEFAULT_CONVEXITY, 0, 100);
    
    auto alg_select = add_combobox("Algorithm", {"Auto", "Goldfeather", "SCS"});
    auto depth_select = add_combobox("Depth Complexity", {"Off", "OcclusionQuery", "On"});
    auto optimization_select = add_combobox("Optimization", { "Default", "ForceOn", "On", "Off" });
    depth_select->Disable();
    
    auto fpstext = new wxStaticText(control_panel, wxID_ANY, "");
    console_sizer->Add(fpstext, 0, wxALL, 5);
    m_canvas->get_fps_counter().add_listener([fpstext](double fps) {
        fpstext->SetLabel(wxString::Format("fps: %.2f", fps) ); 
    });
    
    auto record_btn = new wxToggleButton(control_panel, wxID_ANY, "Record");
    console_sizer->Add(record_btn, 0, wxALL | wxEXPAND, 5);
    
    controlsizer->Add(slider_sizer, 0, wxEXPAND);
    controlsizer->Add(console_sizer, 1, wxEXPAND);
    
    control_panel->SetSizer(controlsizer);
    
    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_canvas.get(), 1, wxEXPAND);
    sizer->Add(control_panel, 0, wxEXPAND);
    SetSizer(sizer);
    
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &){
        RemoveChild(m_canvas.get());
        m_canvas.reset();
        Destroy();
    });    
    
    Bind(wxEVT_MENU, [this](wxCommandEvent &) {
        wxFileDialog dlg(this, "Select project file",  wxEmptyString,
                         wxEmptyString, "*.3mf", wxFD_OPEN|wxFD_FILE_MUST_EXIST);

        if (dlg.ShowModal() == wxID_OK) load_model(dlg.GetPath().ToStdString());
    }, wxID_OPEN);
    
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(true); }, wxID_EXIT);
    
    Bind(wxEVT_SHOW, [this, ms_toggle](wxShowEvent &) {
        const wxSize ClientSize = GetClientSize();
        m_canvas->set_active(ClientSize.x, ClientSize.y);
        enable_multisampling(ms_toggle->GetValue());
    });
    
    Bind(wxEVT_SLIDER, [this, slider](wxCommandEvent &) {
        m_ctl->move_clip_plane(double(slider->GetValue()));
    });
    
    ms_toggle->Bind(wxEVT_TOGGLEBUTTON, [this, ms_toggle](wxCommandEvent &){
        enable_multisampling(ms_toggle->GetValue());
        m_canvas->repaint();
    });
    
    csg_toggle->Bind(wxEVT_TOGGLEBUTTON, [this, csg_toggle](wxCommandEvent &){
        CSGSettings settings = m_canvas->get_csgsettings();
        settings.enable_csg(csg_toggle->GetValue());
        m_canvas->apply_csgsettings(settings);
    });
    
    alg_select->Bind(wxEVT_COMBOBOX,
                     [this, alg_select, depth_select](wxCommandEvent &)
    {
        int sel = alg_select->GetSelection();
        depth_select->Enable(sel > 0);
        CSGSettings settings = m_canvas->get_csgsettings();
        settings.set_algo(OpenCSG::Algorithm(sel));
        m_canvas->apply_csgsettings(settings);
    });
    
    depth_select->Bind(wxEVT_COMBOBOX, [this, depth_select](wxCommandEvent &) {
        int sel = depth_select->GetSelection();
        CSGSettings settings = m_canvas->get_csgsettings();
        settings.set_depth_algo(OpenCSG::DepthComplexityAlgorithm(sel));
        m_canvas->apply_csgsettings(settings);
    });
    
    optimization_select->Bind(wxEVT_COMBOBOX,
                              [this, optimization_select](wxCommandEvent &) {
        int sel = optimization_select->GetSelection();
        CSGSettings settings = m_canvas->get_csgsettings();
        settings.set_optimization(OpenCSG::Optimization(sel));
        m_canvas->apply_csgsettings(settings);
    });
    
    convexity_spin->Bind(wxEVT_SPINCTRL, [this, convexity_spin](wxSpinEvent &) {
        CSGSettings settings = m_canvas->get_csgsettings();
        int c = convexity_spin->GetValue();
        
        if (c > 0) {
            settings.set_convexity(unsigned(c));
            m_canvas->apply_csgsettings(settings);
        }
    });
    
    record_btn->Bind(wxEVT_TOGGLEBUTTON, [this, record_btn](wxCommandEvent &) {
        if (!m_ui_job) {
            m_stbar->set_status_text("No project loaded!");
            return;
        }
        
        if (record_btn->GetValue()) {
            if (m_canvas->camera()) reset(*m_canvas->camera());
            m_ctl->on_scene_updated(*m_scene);
            m_mouse.record(true);
        } else {
            m_mouse.record(false);
            wxFileDialog dlg(this, "Select output file",
                             wxEmptyString, wxEmptyString, "*.events",
                             wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
            
            if (dlg.ShowModal() == wxID_OK) {
                std::fstream stream(dlg.GetPath().ToStdString(),
                                    std::fstream::out);
                
                if (stream.good()) {
                    stream << m_ui_job->get_project_fname() << "\n";
                    m_mouse.save(stream);
                }
            }
        }
    });
    
    // Do the repaint continuously
    m_canvas->Bind(wxEVT_IDLE, [this](wxIdleEvent &evt) {
        if (m_canvas->IsShown()) m_canvas->repaint();
        evt.RequestMore();
    });
    
    bind_canvas_events(m_mouse);
}

void MyFrame::bind_canvas_events(MouseInput &ms)
{
    m_canvas->Bind(wxEVT_MOUSEWHEEL, [&ms](wxMouseEvent &evt) {
        ms.scroll(evt.GetWheelRotation(), evt.GetWheelDelta(),
                  evt.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL ?
                      Slic3r::GL::MouseInput::waVertical :
                      Slic3r::GL::MouseInput::waHorizontal);
    });

    m_canvas->Bind(wxEVT_MOTION, [&ms](wxMouseEvent &evt) {
        ms.move_to(evt.GetPosition().x, evt.GetPosition().y);
    });

    m_canvas->Bind(wxEVT_RIGHT_DOWN, [&ms](wxMouseEvent & /*evt*/) {
        ms.right_click_down();
    });

    m_canvas->Bind(wxEVT_RIGHT_UP, [&ms](wxMouseEvent & /*evt*/) {
        ms.right_click_up();
    });

    m_canvas->Bind(wxEVT_LEFT_DOWN, [&ms](wxMouseEvent & /*evt*/) {
        ms.left_click_down();
    });

    m_canvas->Bind(wxEVT_LEFT_UP, [&ms](wxMouseEvent & /*evt*/) {
        ms.left_click_up();
    });
    
    ms.add_listener(m_ctl);
}

void MyFrame::SLAJob::process() 
{
    using SlStatus = Slic3r::PrintBase::SlicingStatus;
    
    Slic3r::DynamicPrintConfig cfg;
    auto model = Slic3r::Model::read_from_file(m_fname, &cfg);
    
    m_print = std::make_unique<Slic3r::SLAPrint>();
    m_print->apply(model, cfg);
    
    Slic3r::PrintBase::TaskParams params;
    params.to_object_step = Slic3r::slaposHollowing;
    m_print->set_task(params);
    
    m_print->set_status_callback([this](const SlStatus &status) {
        update_status(status.percent, status.text);
    });
    
    try {
        m_print->process();
    } catch(std::exception &e) {
        update_status(0, wxString("Exception during processing: ") + e.what());
    }
}

//int main() {}
