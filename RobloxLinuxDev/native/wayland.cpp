// Reuse the native window/browser bridge without changing Track A.
#define rbx_wayland_api trackb_base_wayland_api
#include "../runtime/browser/wayland.cpp"
#undef rbx_wayland_api
#include <deque>
#include <array>
#include "../runtime/browser/keyboard-map.h"

static GtkIMContext *text_context;
static std::deque<std::string> committed_text;
static bool text_focused;

static bool needs_gtk_text() {
    // Wayland gives the GTK toplevel keyboard focus, not always its SDL child.
    return !current && gtk_window_is_active(GTK_WINDOW(window)) && SDL_GetKeyboardFocus()!=game;
}
static void text_focus() {
    bool focused=needs_gtk_text();
    if(focused==text_focused)return;
    text_focused=focused;
    if(focused)gtk_im_context_focus_in(text_context);
    else {gtk_im_context_reset(text_context);gtk_im_context_focus_out(text_context);committed_text.clear();}
}
static void *create_with_text(int width,int height) {
    void *result=create_window(width,height);
    if(!result || text_context)return result;
    text_context=gtk_im_multicontext_new();
    gtk_im_context_set_client_window(text_context,gtk_widget_get_window(socket_view));
    g_signal_connect(text_context,"commit",G_CALLBACK(+[](GtkIMContext*,const char *text,gpointer){
        if(needs_gtk_text() && text && *text)committed_text.emplace_back(text);
    }),nullptr);
    auto key=+[](GtkWidget*,GdkEventKey *event,gpointer)->gboolean{
        text_focus();
        return text_focused?gtk_im_context_filter_keypress(text_context,event):FALSE;
    };
    g_signal_connect(window,"key-press-event",G_CALLBACK(key),nullptr);
    g_signal_connect(window,"key-release-event",G_CALLBACK(key),nullptr);
    g_signal_connect(window,"focus-out-event",G_CALLBACK(+[](GtkWidget*,GdkEventFocus*,gpointer)->gboolean{
        gtk_im_context_reset(text_context);gtk_im_context_focus_out(text_context);
        text_focused=false;committed_text.clear();return FALSE;
    }),nullptr);
    return result;
}
static int deliver_text(RbxWaylandEvent *event,int ready) {
    if(committed_text.empty())return ready;
    // Synchronous commits belong to keyDown so Roblox snapshots and updates its
    // IME state around insertion. Standalone commits still use insertText:.
    if(ready && (event->type!=RBX_WL_KEY_DOWN || event->text[0]))return ready;
    if(!ready){memset(event,0,sizeof(*event));event->type=RBX_WL_TEXT;}
    // Keep commits longer than the bridge buffer, splitting only at UTF-8 boundaries.
    auto &text=committed_text.front();size_t count=std::min(text.size(),sizeof(event->text)-1);
    while(count<text.size() && (static_cast<unsigned char>(text[count])&0xc0)==0x80)--count;
    memcpy(event->text,text.data(),count);text.erase(0,count);
    if(text.empty())committed_text.pop_front();
    return 1;
}
static int poll_with_text(RbxWaylandEvent *event) {
    if(text_context)text_focus();
    return deliver_text(event,poll_event(event));
}
struct EventBatch {
    std::array<RbxWaylandEvent,64> events;
    size_t next=0,count=0;
};
template<class Events,class Pump,class Poll> static size_t fill_batch(Events &events,Pump drain,Poll poll) {
    // GTK commits/focus changes must precede this batch's IME work.
    drain();size_t count=0;
    while(count<events.size() && poll(&events[count])) {
        int type=events[count++].type;
        // Let the guest observe deactivation before a later focus/input batch.
        if(type==RBX_WL_BLUR || type==RBX_WL_FOCUS)break;
    }
    return count;
}
template<class Fill> static int poll_batch(EventBatch &batch,RbxWaylandEvent *event,Fill fill) {
    if(batch.next==batch.count) {
        // A partial batch already observed the end of SDL's queue. Report it
        // once, so a busy input stream cannot monopolize the guest event loop.
        if(batch.count && batch.count<batch.events.size()){batch.count=batch.next=0;return 0;}
        batch.next=0;batch.count=fill(batch.events);
        if(!batch.count)return 0;
    }
    *event=batch.events[batch.next++];return 1;
}
template<class Request> static size_t receive_batch(std::future<EventBatch> &pending,std::array<RbxWaylandEvent,64> &events,Request request) {
    if(!pending.valid())pending=request();
    // GTK can be busy drawing the profiler or browser. Never make the game
    // wait for that work; keep exactly one request and consume it next poll.
    if(pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return 0;
    auto batch=pending.get();events=std::move(batch.events);return batch.count;
}
extern "C" const RbxWaylandAPI *rbx_wayland_api() {
    auto base=trackb_base_wayland_api();if(!base)return nullptr;
    static RbxWaylandAPI api=*base;
    api.create=[](int w,int h)->void*{return on_ui([=]{return create_with_text(w,h);});};
    api.poll=[](RbxWaylandEvent *event)->int{
        thread_local EventBatch batch;
        thread_local std::future<EventBatch> pending;
        return poll_batch(batch,event,[&](auto &events){
            return receive_batch(pending,events,[]{
                auto task=new std::packaged_task<EventBatch()>([]{
                    EventBatch result{};
                    result.count=fill_batch(result.events,pump,poll_with_text);
                    return result;
                });
                auto future=task->get_future();
                g_main_context_invoke(nullptr,[](gpointer data)->gboolean{
                    std::unique_ptr<std::packaged_task<EventBatch()>> task(static_cast<std::packaged_task<EventBatch()>*>(data));
                    (*task)();return G_SOURCE_REMOVE;
                },task);
                return future;
            });
        });
    };
    return &api;
}
static SDL_Scancode key_scancode(unsigned mac_key) {
    if(mac_key==0xffff)return SDL_SCANCODE_UNKNOWN;
    for(unsigned scan=1;scan<SDL_SCANCODE_COUNT;++scan)
        if(macKey(scan)==mac_key)return static_cast<SDL_Scancode>(scan);
    return SDL_SCANCODE_UNKNOWN;
}
static uint32_t key_character(unsigned mac_key,unsigned carbon_modifiers) {
    auto scan=key_scancode(mac_key);if(scan==SDL_SCANCODE_UNKNOWN)return 0;
    SDL_Keymod mods=((carbon_modifiers&1)?SDL_KMOD_GUI:0)|((carbon_modifiers&2)?SDL_KMOD_SHIFT:0)|
        ((carbon_modifiers&4)?SDL_KMOD_CAPS:0)|((carbon_modifiers&8)?SDL_KMOD_ALT:0)|
        ((carbon_modifiers&16)?SDL_KMOD_CTRL:0);
    uint32_t key=SDL_GetKeyFromScancode(scan,mods,false);
    return key<=0x10ffff && !(key>=0xd800 && key<=0xdfff)?key:0;
}
extern "C" uint32_t rbx_wayland_key_character(unsigned mac_key,unsigned carbon_modifiers) {
    static const bool ready=rbx_wayland_api()!=nullptr;
    if(!ready)return 0;
    return on_ui([=]{return initialize_sdl_video()?key_character(mac_key,carbon_modifiers):0u;});
}
#ifdef TRACKB_WAYLAND_CHECK
#include <cassert>
int main() {
    int w=0,h=0;
    assert(parse_render_size("1512x949",w,h) && w==1512 && h==949);
    for(auto invalid:{"", "0x949", "1512x0", "1512x949junk", "1512", "-1x949", "999999999999999999x949", "8193x949"})
        assert(!parse_render_size(invalid,w,h));
    assert(!parse_render_size(nullptr,w,h));
    // A compositor can keep the same allocation after rejecting a guest resize.
    // Both polls must re-announce it, and each request is consumed exactly once.
    game=reinterpret_cast<SDL_Window*>(uintptr_t(1));
    for(int request=0;request<2;++request) {
        resize_report_pending=true; RbxWaylandEvent event{};
        assert(poll_event(&event)==1 && event.type==RBX_WL_RESIZE);
        assert(event.x==content_width && event.y==content_height && !resize_report_pending);
    }
    game=nullptr;
    // Reproduce a terminal resize delivered to an unregistered native thread.
    struct sigaction old_resize{},old_term{},now{};
    assert(sigaction(SIGWINCH,nullptr,&old_resize)==0);
    assert(sigaction(SIGTERM,nullptr,&old_term)==0);
    assert(signal(SIGWINCH,+[](int){_exit(90);})!=SIG_ERR);
    assert(ignore_terminal_resize());
    std::thread([]{assert(pthread_kill(pthread_self(),SIGWINCH)==0);}).join();
    assert(sigaction(SIGWINCH,nullptr,&now)==0 && now.sa_handler==SIG_IGN);
    assert(sigaction(SIGTERM,nullptr,&now)==0 && now.sa_handler==old_term.sa_handler);
    assert(sigaction(SIGWINCH,&old_resize,nullptr)==0);
    assert(compatible_user_agent("Mozilla/5.0 Roblox/DarwinRobloxApp/0.738 (GlobalDist)")=="Mozilla/5.0 Roblox/Darwin RobloxApp/0.738 (GlobalDist)");
    assert(compatible_user_agent("CustomProbe/1.0")=="CustomProbe/1.0");
    assert(compatible_user_agent("Mozilla/5.0 (X11; Linux x86_64) Roblox/DarwinRobloxApp/0.738")=="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Roblox/Darwin RobloxApp/0.738");
    assert(request_header("X-Roblox-Test","value") && request_header("User-Agent","Native/1.0"));
    assert(!request_header("Host","evil") && !request_header("X-Bad\n","value") && !request_header("X-Test","bad\r\nnext"));
    assert(rbx_client_url("roblox://experiences/start?placeId=1&gameInstanceId=server"));
    assert(rbx_client_url("roblox-player:1+launchmode:play+gameinfo:test"));
    assert(rbx_client_url("ROBLOX://placeId=1"));
    for(auto url:{"", "roblox:", "roblox-player:", "https://roblox.com/games/1", "roblox-extra:test", "file:///etc/passwd", "roblox:test\nnext"})assert(!rbx_client_url(url));
    assert(!rbx_client_url(nullptr) && !rbx_client_url((std::string("roblox:")+std::string(16384,'x')).c_str()));
    // No video initialization: exercise the actual SDL FIFO and capture filter.
    assert(SDL_Init(SDL_INIT_EVENTS));
    // Event delivery and label lookup must use the same physical key mapping.
    for(unsigned scan=1;scan<SDL_SCANCODE_COUNT;++scan)
        if(macKey(scan)!=0xffff)assert(key_scancode(macKey(scan))==scan);
    assert(key_scancode(0xffff)==SDL_SCANCODE_UNKNOWN && key_scancode(128)==SDL_SCANCODE_UNKNOWN);
    assert(key_scancode(10)==SDL_SCANCODE_NONUSBACKSLASH);
    assert(key_character(16,0)=='y' && key_character(14,0)=='e' && key_character(44,0)=='/');
    assert(key_character(16,2)=='Y' && key_character(16,4)=='Y' && key_character(16,6)=='y');
    assert(key_character(16,0x100)=='y' && key_character(122,0)==0 && key_character(0xffff,0)==0);
    // Native GDK notifications annotate existing clicks, not extra presses.
    GdkEvent clicks[8]{};
    const GdkEventType click_types[]={GDK_BUTTON_PRESS,GDK_BUTTON_RELEASE,GDK_BUTTON_PRESS,GDK_2BUTTON_PRESS,
                                     GDK_BUTTON_RELEASE,GDK_BUTTON_PRESS,GDK_3BUTTON_PRESS,GDK_BUTTON_RELEASE};
    const guint32 click_times[]={10,20,30,30,40,50,50,60};
    for(unsigned i=0;i<8;++i){
        clicks[i].button.type=click_types[i];clicks[i].button.button=1;clicks[i].button.time=click_times[i];
        clicks[i].button.x=12;clicks[i].button.y=34;
    }
    for(unsigned i=0;i<8;++i){
        SDL_Event translated{};
        if(translate_gtk_button(clicks[i].button,i+1<8?&clicks[i+1]:nullptr,translated.button))assert(SDL_PushEvent(&translated));
    }
    SDL_Event translated{};unsigned click_events=0;
    while(SDL_PeepEvents(&translated,1,SDL_GETEVENT,SDL_EVENT_FIRST,SDL_EVENT_LAST)==1){
        assert(click_events<6);
        assert(translated.type==(click_events%2?SDL_EVENT_MOUSE_BUTTON_UP:SDL_EVENT_MOUSE_BUTTON_DOWN));
        assert(translated.button.down==(click_events%2==0));
        assert(translated.button.clicks==click_events/2+1 && translated.button.x==12 && translated.button.y==34);
        ++click_events;
    }
    assert(click_events==6);
    GdkEventButton other=clicks[0].button;other.button=3;
    assert(translate_gtk_button(other,nullptr,translated.button) && translated.button.clicks==1);
    assert(translate_gtk_button(clicks[7].button,nullptr,translated.button) && translated.button.clicks==3);
    // A notification for another press must not annotate this one.
    assert(translate_gtk_button(clicks[0].button,&clicks[3],translated.button) && translated.button.clicks==1);
    GdkEventScroll scroll{};scroll.x=7;scroll.y=9;
    const GdkScrollDirection directions[]={GDK_SCROLL_RIGHT,GDK_SCROLL_LEFT,GDK_SCROLL_UP,GDK_SCROLL_DOWN,GDK_SCROLL_SMOOTH};
    const float scroll_x[]={1,-1,0,0,2},scroll_y[]={0,0,1,-1,-3};
    scroll.delta_x=2;scroll.delta_y=3;
    for(unsigned i=0;i<5;++i){
        scroll.direction=directions[i];translate_gtk_scroll(scroll,translated.wheel);
        assert(translated.type==SDL_EVENT_MOUSE_WHEEL && translated.wheel.x==scroll_x[i] && translated.wheel.y==scroll_y[i]);
        assert(translated.wheel.mouse_x==7 && translated.wheel.mouse_y==9);
    }
    raw_motion_event=SDL_RegisterEvents(2);assert(raw_motion_event);
    capture_state_event=raw_motion_event+1;
    auto motion=[](Uint32 type,float dx,float dy){
        SDL_Event event{};event.type=type;event.motion.xrel=dx;event.motion.yrel=dy;
        assert(SDL_PushEvent(&event));
    };
    motion(SDL_EVENT_MOUSE_MOTION,3,-1.5);
    queue_capture_state(true);assert(!capture_cursor_visible());
    motion(SDL_EVENT_MOUSE_MOTION,99,99); // duplicate absolute while locked
    motion(raw_motion_event,1.25,-2.5);
    SDL_Event release{};release.type=SDL_EVENT_MOUSE_BUTTON_UP;release.button.button=3;
    assert(SDL_PushEvent(&release));
    // Unlock before consuming: earlier raw motion must survive, and the earlier
    // absolute event must use the state at its own position in the FIFO.
    queue_capture_state(false);assert(capture_cursor_visible());
    motion(SDL_EVENT_MOUSE_MOTION,2,4);
    const Uint32 ordered[]={SDL_EVENT_MOUSE_MOTION,raw_motion_event,SDL_EVENT_MOUSE_BUTTON_UP,SDL_EVENT_MOUSE_MOTION};
    unsigned received=0;double motion_x=0,motion_y=0;SDL_Event queued{};
    while(SDL_PeepEvents(&queued,1,SDL_GETEVENT,SDL_EVENT_FIRST,SDL_EVENT_LAST)==1) {
        if(discard_pointer_event(queued))continue;
        assert(received<4 && queued.type==ordered[received++]);
        if(queued.type!=SDL_EVENT_MOUSE_BUTTON_UP){motion_x+=queued.motion.xrel;motion_y+=queued.motion.yrel;}
    }
    assert(received==4 && motion_x==6.25 && motion_y==0 && !queued_capture_active);
    // A persistent constraint can acknowledge lock again after compositor unlock.
    queue_capture_state(true);assert(!capture_cursor_visible());
    cursor_visible_requested=false;
    queue_capture_state(false);assert(!capture_cursor_visible());
    cursor_visible_requested=true;assert(capture_cursor_visible());
    while(SDL_PeepEvents(&queued,1,SDL_GETEVENT,SDL_EVENT_FIRST,SDL_EVENT_LAST)==1)assert(discard_pointer_event(queued));
    assert(!queued_capture_active);SDL_Quit();
    PointerCapture capture;
    capture.request(true);assert(!capture.requested); // stale request while unfocused
    assert(capture.focus(true));capture.request(true);assert(capture.requested);
    // RMB-up does not revoke Shift Lock. Only an engine release does.
    assert(!capture.focus(true) && capture.requested);
    capture.request(false);assert(!capture.requested);
    capture.request(true);assert(capture.focus(false) && !capture.requested);
    capture.request(true);assert(!capture.requested); // queued request after browser/blur
    assert(capture.focus(true) && !capture.requested); // no automatic recapture
    // Exercise the release helper without creating a display or real constraint.
    // Socket coordinates include a titlebar offset on the parent lock surface.
    unsigned hint_step=0;
    wl_fixed_t hint_x=0,hint_y=0;
    auto hint=[&](wl_fixed_t x,wl_fixed_t y){assert(hint_step++==0);hint_x=x;hint_y=y;};
    auto commit=[&]{assert(hint_step++==1);};
    int fake_lock;
    locked_pointer=reinterpret_cast<zwp_locked_pointer_v1*>(&fake_lock);
    pointer_capture.focus(true);pointer_capture.request(true);
    pointer_x=3;pointer_y=4;pointer_position_known=true;
    assert(capture_release_hint(100.25,50.5,7,32,hint,commit));
    assert(hint_step==2 && wl_fixed_to_double(hint_x)==107.25 && wl_fixed_to_double(hint_y)==82.5);
    assert(capture_x==100.25f && capture_y==50.5f && pointer_x==capture_x && pointer_y==capture_y);
    assert(!pointer_position_known && locked_pointer); // Commit precedes caller's unlock.
    hint_step=0;pointer_capture.focus(false);
    assert(!capture_release_hint(9,10,0,0,hint,commit) && hint_step==0);
    assert(pointer_x==100.25f && pointer_y==50.5f); // No unfocused logical warp either.
    pointer_capture.focus(true);
    assert(!capture_release_hint(INFINITY,10,0,0,hint,commit) && hint_step==0);
    assert(!capture_release_hint(8388608,10,0,0,hint,commit) && hint_step==0);
    locked_pointer=nullptr;
    assert(!capture_release_hint(9,10,0,0,hint,commit) && hint_step==0); // SDL fallback belongs to action().
    pointer_capture.focus(false);pointer_x=pointer_y=capture_x=capture_y=0;pointer_position_known=false;
    EventBatch batch;unsigned produced=0,refills=0,drains=0;
    auto fill=[&](auto &events){
        ++refills;
        return fill_batch(events,[&]{++drains;},[&](auto *event){
            assert(drains==refills); // Drain precedes every batch's first event.
            if(produced==150)return 0;
            *event={};event->type=produced%2?RBX_WL_UP:RBX_WL_DOWN;
            event->x=produced++;return 1;
        });
    };
    RbxWaylandEvent item{};
    for(unsigned i=0;i<150;++i){
        assert(poll_batch(batch,&item,fill)==1 && item.x==i);
        assert(item.type==(i%2?RBX_WL_UP:RBX_WL_DOWN));
    }
    assert(poll_batch(batch,&item,fill)==0 && refills==3 && drains==3);
    assert(poll_batch(batch,&item,fill)==0 && refills==4);
    EventBatch boundary;unsigned at=0;
    const int sequence[]={RBX_WL_MOTION,RBX_WL_UP,RBX_WL_BLUR,RBX_WL_FOCUS,RBX_WL_MOTION};
    auto transitions=[&](auto &events){return fill_batch(events,[]{},[&](auto *event){
        if(at==5)return 0;*event={};event->type=sequence[at++];event->dx=1.25;event->dy=-2.5;return 1;
    });};
    double dx=0,dy=0;
    for(int expected:sequence) {
        int ready=poll_batch(boundary,&item,transitions);
        if(!ready)ready=poll_batch(boundary,&item,transitions);
        assert(ready && item.type==expected);
        if(item.type==RBX_WL_MOTION){dx+=item.dx;dy+=item.dy;}
        if(item.type==RBX_WL_BLUR)assert(at==3);
        if(item.type==RBX_WL_FOCUS)assert(at==4);
    }
    assert(dx==2.5 && dy==-5.0);
    // A busy GTK thread must not block polling, duplicate requests, or lose
    // a batch when the guest has already returned from the initiating poll.
    std::future<EventBatch> pending;
    std::promise<EventBatch> delivery;
    unsigned requests=0;
    auto request=[&]{++requests;return delivery.get_future();};
    std::array<RbxWaylandEvent,64> events{};
    assert(receive_batch(pending,events,request)==0);
    assert(receive_batch(pending,events,request)==0 && requests==1);
    EventBatch delivered{};delivered.count=2;
    delivered.events[0].type=RBX_WL_DOWN;delivered.events[1].type=RBX_WL_UP;
    delivery.set_value(delivered);
    assert(receive_batch(pending,events,request)==2 && requests==1 && !pending.valid());
    assert(events[0].type==RBX_WL_DOWN && events[1].type==RBX_WL_UP);
    const std::string input=std::string(254,'a')+"é🙂"+std::string(300,'b');
    committed_text.push_back(input);committed_text.emplace_back("second");
    std::string output;
    while(!committed_text.empty()) {
        RbxWaylandEvent event{};assert(deliver_text(&event,0)==1);
        assert(event.type==RBX_WL_TEXT && g_utf8_validate(event.text,-1,nullptr));
        output+=event.text;
    }
    assert(output==input+"second");
    committed_text.emplace_back("key");
    RbxWaylandEvent event{};event.type=RBX_WL_UP;
    assert(deliver_text(&event,1)==1 && committed_text.size()==1);
    event.type=RBX_WL_KEY_DOWN;
    assert(deliver_text(&event,1)==1 && event.type==RBX_WL_KEY_DOWN && std::string(event.text)=="key");
    assert(committed_text.empty());
    // Native visibility is independent of presentation ownership. The CGL
    // gate controls ownership so the first fallback GL swap remains possible.
    can_present=true;
    assert(visible() && rbx_wayland_vulkan_visible());
    can_present=false;
    assert(!visible() && !rbx_wayland_vulkan_visible());
    can_present=true;
    puts("PASS keyboard label mapping/modifiers, GTK click counts/scroll signs, capture FIFO/unlock/cursor state, capture requests, release hint offset/commit/focus safety, blur/browser release, focus batch barriers, 1:1 deltas, 150 ordered events, UTF-8 IME and shared GL/Vulkan window visibility");
}
#endif
