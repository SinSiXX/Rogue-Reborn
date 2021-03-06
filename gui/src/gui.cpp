/*
Rogue Reborn GUI "Gui" source from src/gui.cpp 

Copyright 2010 Justin Bruce Van Horne.  All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <memory>
#include <fstream>
#include <map>
#include <string>
#include <iterator>
#include <sstream>
#include <v8.h>
#include <hat/gui/gui.hpp>
#include <hat/gui/image.hpp>
#include <hat/gui/eventful.hpp>
#include <hat/engine/q_shared.h>
#include <hat/gui/ui_public.h>
#include <hat/gui/ui_local.h>

#define EXCEPTION(file, line, message)													\
{																						\
    std::stringstream new_exception(std::stringstream::in | std::stringstream::out);	\
    new_exception << message;															\
    const std::string& exception_str = new_exception.str();								\
    current_exception = Gui_exception(file, line, exception_str);						\
}

namespace hat {

namespace {
/*
This is a list of functions made available through the Gui class.
Any additional functions that are added to the class should be added here.
They will be looped over and added to the function list when a template
is constructed.

If they are marked as `internal` (denoted by true) then they will be
available in the `gui.internal` namespace.
*/
JS_mapping accessors[] = {
    { NULL, NULL, NULL } // Signals the end of the accessor list
};

/*
This is a list of functions made availabe through the Gui class.
Any additional functions that are addeed tot he class should be added here.
They will be looped over and added to the function list when a template
is constructued.
*/
JS_fun_mapping funs[] = {
    JS_FUN_MAP_INTERNAL(Gui, setup_menus),
    JS_FUN_MAP(Gui, think),
    JS_FUN_MAP(Gui, log),
    JS_CLASS_INVOCATION_CUSTOM(Image, "SimpleImage"),
    JS_CLASS_INVOCATION_CUSTOM(Eventful_image, "Image"),
    { NULL, NULL, NULL } // Signlas the end of the function list
};

/*
This is a mapping for menus and corresponding scripts.
*/
typedef std::map<int, std::string> Menu_mapping;
typedef std::map<std::string, int> Engine_mapping;

const char* UIMENUS[] = {
    "UIMENU_SHUTDOWN",
    "UIMENU_MAIN",
    "UIMENU_INGAME",
    "UIMENU_TEAM",
    "UIMENU_POSTGAME"
};

Menu_mapping available_menus;
Engine_mapping engine_menus;

}

/*
The constructor provides the mininum amount of work needed to
instantiate an instance of the desired JavaScript source. It'll load
the GUI namespace, compile, and run the JavaScript code. If there are
any problems along the way, then an exception will occur that can be
checked from in_exception_state() method.
*/
Gui::Gui(const char* js_file)
    : js_filename(js_file), cursor_handle(-1)
{
    // Assign a binding for the screen attributes
    screen = &global::screen_attrs;

    // Static menus that are a must
    if (engine_menus.empty()) {
        engine_menus["shutdown"] = UIMENU_NONE;
        engine_menus["main"] = UIMENU_MAIN;
        engine_menus["ingame"] = UIMENU_INGAME;
        engine_menus["team"] = UIMENU_TEAM;
        engine_menus["postgame"] = UIMENU_POSTGAME;
    }

    Com_Printf("Gui()::Gui() - %s\n", js_file);

    // Read the file into a buffer for script conversion
    fileHandle_t handle;
    trap_FS_FOpenFile(js_file, &handle, FS_READ);
    if (!handle) {
        EXCEPTION(__FILE__, __LINE__, js_file << " could not be opened/read");
        return;
    }

    const unsigned int file_size = trap_FS_FileSize(js_file);
    std::auto_ptr<char> file_raw(new char[file_size]);
    memset(file_raw.get(), '\0', sizeof(char) * file_size);

    // Parse the data buffer
    trap_FS_Read(file_raw.get(), file_size, handle);
    trap_FS_FCloseFile(handle);
  
    // An execution scope is necessary for temporary references
    v8::HandleScope execution_scope;
    v8::Handle<v8::String> script = v8::String::New(file_raw.get(), file_size);
    global_scope = v8::ObjectTemplate::New();
    global_scope->SetInternalFieldCount(1);

    // Create our execution context (global context)
    global_context = v8::Context::New(NULL, global_scope);
    v8::Context::Scope context_scope(global_context);

    // Create the gui namespace template and initialize the namespace
    gui_tmpl = v8::Persistent<v8::FunctionTemplate>();
    gui_ns = wrap_tmpl(&gui_tmpl, this, NULL);
    v8::Handle<v8::Object> global_proxy = global_context->Global();
    v8::Handle<v8::Object> global = global_proxy->GetPrototype().As<v8::Object>();
    assert(global->InternalFieldCount() == 1);
    assert(global->GetPointerFromInternalField(0) == NULL);
    global->SetPointerInInternalField(0, this);
    global->Set(v8::String::New("gui"), gui_ns);

    // Setup the error handling and compile the current script
    Com_Printf("Compiling...\n");
    v8::TryCatch compile_try_catch;
    v8::Handle<v8::Script> compiled_script = v8::Script::Compile(script);
    if (compile_try_catch.HasCaught()) {
        v8::Handle<v8::Message> e = compile_try_catch.Message();
        EXCEPTION(js_file, e->GetLineNumber(), *v8::String::Utf8Value(e->Get()));
        return;
    }

    // Now run the script and check for errors.
    Com_Printf("Running...\n");
    v8::TryCatch run_try_catch;
    v8::Handle<v8::Value> compiled_result = compiled_script->Run();
    if (run_try_catch.HasCaught()) {
        v8::Handle<v8::Message> e = run_try_catch.Message();
        EXCEPTION(js_file, e->GetLineNumber(), *v8::String::Utf8Value(e->Get()));
        return;
    }

    Com_Printf("Success...\n");
    cursor_handle = trap_R_RegisterShaderNoMip("gfx/2d/cursors/Azenis/Arrow.png");
    last_game_msec = trap_Milliseconds();
}

Gui::~Gui()
{
    shutdown();
}

/*
The think functions are the list of methods that are assigned to be called
once per frame. The methods are passed to the gui.think() function. These
act similar to jQuery's $.ready();
*/
void Gui::think_fun()
{
    if (!gui_think_funs.size()) return;

    // Important, we always need to know where we are at in terms of the
    // execution scope and context scope.
    v8::HandleScope execution_scope;
    v8::Context::Scope context_scope(global_context);

    const Gui_kbm& kbm_state = last_kbm_state;

    // These values will never change; they are just symbols
    static const v8::Local<v8::String> kbm_argv_mx_str	 = v8::String::NewSymbol("mx");
    static const v8::Local<v8::String> kbm_argv_my_str	 = v8::String::NewSymbol("my");
    static const v8::Local<v8::String> kbm_argv_key_str	 = v8::String::NewSymbol("key");
    static const v8::Local<v8::String> kbm_argv_down_str = v8::String::NewSymbol("down");
    static const v8::Local<v8::String> kbm_argv_held_str = v8::String::NewSymbol("held");

    // These will change; they are the KBM state.
    v8::Handle<v8::Object> kbm_argv = v8::Object::New();
    kbm_argv->Set(kbm_argv_mx_str, v8::Int32::New(kbm_state.mx));
    kbm_argv->Set(kbm_argv_my_str, v8::Int32::New(kbm_state.my));
    kbm_argv->Set(kbm_argv_key_str, v8::Int32::New(kbm_state.key));
    kbm_argv->Set(kbm_argv_down_str, v8::Boolean::New(kbm_state.down));

    // The two arguments to the GUI think() are the timer and kbm
    v8::Handle<v8::Value> argvs[2] = {
        v8::Int32::New(1000), kbm_argv
    };

    // We're in the case that the args don't exist, so now we are calling 
    // the various methods of the think()
    v8::TryCatch run_try_catch;
    for (Function_list::const_iterator tci = gui_think_funs.begin();
        tci != gui_think_funs.end();
        ++tci)
    {
        (*tci)->Call(global_context->Global(), 2, argvs);
        if (run_try_catch.HasCaught()) {
            v8::Handle<v8::Message> e = run_try_catch.Message();
            EXCEPTION(js_filename, e->GetLineNumber(), *v8::String::Utf8Value(e->Get()));
            return;
        }
    }
}

/*
The logic of this GUI. Everything from controlling what gets rendered to
what runs and doesn't run. It's a powerful method.
*/
void Gui::think(const Gui_kbm& kbm_state)
{
    Gui_kbm old_state = last_kbm_state;
    last_kbm_state = kbm_state;

    // Clear the color buffer
    trap_R_SetColor(NULL);

    // Add any elements that were created during the last execution
    std::copy(pending_elements.begin(), 
              pending_elements.end(), 
              std::back_inserter(available_elements));
    pending_elements.clear();

    // Call the thinks for our GUI
    think_fun();

    // Iterate through the elements and do our thinks and checks
    const int mx = kbm_state.mx;
    const int my = kbm_state.my;
    const int dx = kbm_state.dx;
    const int dy = kbm_state.dy;
    for (Element_list::iterator iter = available_elements.begin();
        iter != available_elements.end();
        ++iter)
    {
        Element* e = *iter;
        e->think(trap_Milliseconds());

        Eventful* eventful_e = dynamic_cast<Eventful*>(e);
        if (!eventful_e) continue;

        bool in_bounds = e->check_bounds(kbm_state.mx, kbm_state.my);
        bool in_drag = eventful_e->eventful_attrs.is_mouse_drag;
        bool no_interaction = !in_bounds && !in_drag;

        // If we were previously in a mouse_over event and we are
        // no longer in the bounds of the element, then we are
        // in a mouse_out state
        if (eventful_e->eventful_attrs.is_mouse_over && no_interaction) {
            eventful_e->mouse_out(mx, my);
            eventful_e->finish_state();
            continue;
        } 

        // If we are not in bounds, then we can just finish up
        // and clean our state. This is important to do.
        if (no_interaction) {
            eventful_e->finish_state();
            continue;
        }

        // We can't really click or have any other events without first
        // calling the mouse over -- We also don't want to be calling
        // the mouse_over event when we are dragging.
        if (in_bounds && !in_drag) {
            eventful_e->mouse_over(mx, my);
        }
        
        // If we have clicked some mouse button between a series of buttons
        if (kbm_state.button >= GB_LEFT_CLICK && kbm_state.button <= GB_NEXT_CLICK) {
            // New events are going to allow us to have a either a mouse_down or mouse_up
            // It prevents us from having multiple events of single-click events
            if (kbm_state.new_event) {
                if (kbm_state.down) {
                    eventful_e->mouse_down(mx, my, kbm_state.button);
                // Otherwise, we are releasing our mouse button and
                // calling the mouse up event now
                } else if (!kbm_state.down) {
                    eventful_e->mouse_up(mx, my, kbm_state.button);
                } 
            // Otherwise we are dragging
            } else if (eventful_e->is_draggable(250)) {
                eventful_e->mouse_drag(e->element_attrs.x + dx, e->element_attrs.y + dy, kbm_state.button);
            } else if (eventful_e->eventful_attrs.is_mouse_down) {
                Com_Printf("Updating element held_delta: %d     %d\n", kbm_state.held_delta, eventful_e->eventful_attrs.held_timer);
                eventful_e->update_held_timer(kbm_state.held_delta);
            }
        // If we are scrolling, then we have two separate events to
        // check for -- both of which are unique
        } else if (kbm_state.button == GB_SCROLL_UP) {
            eventful_e->scroll_up();
        } else if (kbm_state.button == GB_SCROLL_DOWN) {
            eventful_e->scroll_down();
        }

        // This is important, it lets the eventful element know that we
        // are done manipulating the state of the object for this pass.
        eventful_e->finish_state();
    }

    // Draw the cursor and update the internal timer
    if (screen->modified) {
        cursor_handle = trap_R_RegisterShaderNoMip("gfx/2d/cursors/Azenis/Arrow.png");
    }

    // Adjust the image like we would an actual element
    trap_R_DrawStretchPic(
        kbm_state.mx * screen->scale_x, 
        kbm_state.my * screen->scale_y,
        50 * screen->scale_x, 
        50 * screen->scale_y, 
        0, 0, 1, 1, cursor_handle);
    last_game_msec = trap_Milliseconds();
}

void Gui::shutdown()
{
    // Be nice and free memory for the current elements
    for (Element_list::iterator iter = available_elements.begin();
        iter != available_elements.end();
        ++iter)
    {
        (*iter)->cleanup();
        delete (*iter);
    }

    // Be nice and free memory for the active elements
    for (Element_list::iterator iter = pending_elements.begin();
        iter != pending_elements.end();
        ++iter)
    {
        (*iter)->cleanup();
        delete (*iter);
    }
}

bool Gui::in_exception_state()
{
    return current_exception.line != -1;
}

const Gui_exception& Gui::exception()
{
    return current_exception;
}

void Gui::engine_menu_clear()
{
    available_menus.clear();
    engine_menus.clear();
}

const char* Gui::engine_menu_exists(const int menu)
{
    Menu_mapping::const_iterator menu_name = available_menus.find(menu);
    if (menu_name == available_menus.end()) {
        return NULL;
    }
    return menu_name->second.c_str();
}

const char* Gui::engine_menu_name(const int menu)
{
    return UIMENUS[menu];
}

JS_FUN_CLASS(Gui, setup_menus)
{
    v8::Local<v8::Object> menu_obj = args[0]->ToObject();
    v8::Local<v8::Array> menu_to_be_set = menu_obj->GetPropertyNames();
    Com_Printf("Gui()::setup_menus()\n");

    Gui* gui = unwrap<Gui>(args.Holder());
    if (!gui) {
        return v8::Exception::Error(v8::String::New("Gui class has become detached?"));
    }

    for (int i = 0; i < menu_to_be_set->Length(); i++) {
        const v8::Local<v8::String> real_key = menu_to_be_set->Get(v8::Int32::New(i))->ToString();
        const std::string key = *v8::String::Utf8Value(real_key);
        std::stringstream val(std::stringstream::in | std::stringstream::out);
        val << "javascript/gui/" << *v8::String::Utf8Value(menu_obj->Get(real_key));

        Engine_mapping::const_iterator cit = engine_menus.find(key);
        if (cit == engine_menus.end()) {
            return v8::Exception::RangeError(v8::String::New("Attempted to set the value of a non-existent engine menu"));
        }

        available_menus[cit->second] = val.str();
    }

    return v8::Undefined();
}

/*
A new think() call has been made. This means that we are either calling
think without arguments (implied to call the methods themselves) or we
are adding a new function to the list.
*/
JS_FUN_CLASS(Gui, think)
{
    v8::Local<v8::Object> menu_obj = args[0]->ToObject();
    Gui* gui = unwrap<Gui>(args.Holder());

    if (!gui) {
        return v8::Exception::Error(v8::String::New("Gui class has become detached?"));
    }

    // We have an actual argument to the function, we are going to need
    // to check if the argument is a function and if it is, we append
    // the function to the function list
    if (args.Length() > 0) {
        v8::Handle<v8::Value> think_val = args[0];
        if (think_val->IsFunction()) {
            v8::Handle<v8::Function> think_fun = v8::Handle<v8::Function>::Cast(think_val);
            gui->gui_think_funs.push_back(v8::Persistent<v8::Function>::New(think_fun));
            return v8::Context::GetCalling()->Global();
        } else {
            return v8::Exception::TypeError(v8::String::New("Expected a function, but got something else."));
        }
    }

    gui->think_fun();
    return v8::Context::GetCalling()->Global();
}

/*
This is a simple logger that will print messages to the console.
*/
JS_FUN_CLASS(Gui, log)
{
    static int counter = 0;

    if (args.Length()) {
        for (int i = 0; i < args.Length(); i++) {
            const char* message = strdup(*v8::String::Utf8Value(args[i]->ToString()));
            Com_Printf("<GUI::%d>: %s\n", counter++, message);
            free((void*)message);
        }
    }

    return v8::Undefined();
}

/*
The only way to expose classes to JavaScript natively is to wrap the
existing object. To properly due this, we have to ensure that the object
remains in memory during the execution of script. This responsibility
is dictated by the container, which in this case is the active Gui.
*/
v8::Handle<v8::Object> Gui::wrap_tmpl(
    v8::Handle<v8::FunctionTemplate>* tmpl, 
    Gui* e, 
    Object_template_extension extension)
{
    v8::HandleScope handle_scope;

    if (tmpl->IsEmpty()) {
        (*tmpl) = v8::FunctionTemplate::New();
    }

    (*tmpl)->SetClassName(v8::String::New("Gui"));
    // We only need to create the template once.
    generate_fun_tmpl(tmpl, accessors, funs, NULL);
    v8::Handle<v8::Function> gui_ctor = (*tmpl)->GetFunction();
    v8::Local<v8::Object> obj = gui_ctor->NewInstance();
    obj->SetInternalField(0, v8::External::New(e));
    return handle_scope.Close(obj);
}

}
