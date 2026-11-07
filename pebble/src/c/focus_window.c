#include "focus_window.h"
#include "protocol.h"
#include "task_view_window.h"

// Launch screen for the focus set: the 3 unfinished tasks the phone
// selected for the day (Settings -> Pins). Rows arrive as standard task
// chunks; the window holds at most 3 so it stays tiny even on aplite.

#define MAX_FOCUS_ITEMS 3

static Window *s_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_loading_layer;
static UiItem s_items[MAX_FOCUS_ITEMS];
static int s_count = 0;
static bool s_loading = false;
static int s_expected_chunks = 0;
static uint32_t s_received_mask = 0;
static uint8_t s_request_txn = 0;
static AppTimer *s_timer = NULL;

static void chunk_timeout_handler(void *data) {
    s_timer = NULL;
    if (s_loading) {
        s_loading = false;
        layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
        layer_set_hidden(menu_layer_get_layer(s_menu_layer), false);
        menu_layer_reload_data(s_menu_layer);
    }
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *data) {
    if (s_loading) return 0;
    return s_count > 0 ? (uint16_t)s_count : 1;
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    return 44;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
    if (s_count == 0) {
        menu_cell_basic_draw(ctx, cell_layer, "All done. Nothing pinned.", NULL, NULL);
        return;
    }
    int row = cell_index->row;
    if (row >= s_count) {
        menu_cell_basic_draw(ctx, cell_layer, "...", NULL, NULL);
        return;
    }
    menu_cell_basic_draw(ctx, cell_layer, s_items[row].title, NULL, NULL);
}

static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    int row = cell_index->row;
    if (s_count > 0 && row < s_count) {
        task_view_window_push(s_items[row].id_high, s_items[row].id_low);
    }
}

static void window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
        .get_num_rows = get_num_rows,
        .get_cell_height = get_cell_height,
        .draw_row = draw_row,
        .select_click = select_click,
    });
    menu_layer_set_click_config_onto_window(s_menu_layer, window);
#ifdef PBL_COLOR
    menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
    menu_layer_set_highlight_colors(s_menu_layer, GColorCobaltBlue, GColorWhite);
#endif
    layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

    s_loading_layer = text_layer_create(
        GRect(0, bounds.size.h / 2 - 15, bounds.size.w, 30));
    text_layer_set_text(s_loading_layer, "Loading...");
    text_layer_set_text_alignment(s_loading_layer, GTextAlignmentCenter);
    text_layer_set_font(s_loading_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    layer_add_child(window_layer, text_layer_get_layer(s_loading_layer));

    s_count = 0;
    s_loading = true;
    layer_set_hidden(menu_layer_get_layer(s_menu_layer), true);
    layer_set_hidden(text_layer_get_layer(s_loading_layer), false);

    if (protocol_send_get_focus()) {
        s_request_txn = protocol_get_active_transaction_id();
        s_expected_chunks = 0;
        s_received_mask = 0;
        s_timer = app_timer_register(5000, chunk_timeout_handler, NULL);
    } else {
        chunk_timeout_handler(NULL);
    }
}

static void window_unload(Window *window) {
    if (s_timer) {
        app_timer_cancel(s_timer);
        s_timer = NULL;
    }
    menu_layer_destroy(s_menu_layer);
    text_layer_destroy(s_loading_layer);
    s_loading = false;
    window_destroy(s_window);
    s_window = NULL;
}

void focus_window_push(void) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(s_window, true);
}

void focus_window_handle_response(DictionaryIterator *iter) {
    if (!s_window) return;

    Tuple *txn_t = dict_find(iter, KEY_TRANSACTION_ID);
    if (txn_t && (uint8_t)protocol_tuple_value(txn_t) != s_request_txn) return;

    if (!s_loading) return;

    Tuple *chunk_count_t = dict_find(iter, KEY_CHUNK_COUNT);
    s_expected_chunks = chunk_count_t ? (int)protocol_tuple_value(chunk_count_t) : 1;

    Tuple *chunk_index_t = dict_find(iter, KEY_CHUNK_INDEX);
    int chunk_index = chunk_index_t ? (int)protocol_tuple_value(chunk_index_t) : 0;
    if (chunk_index < 0 || chunk_index >= MAX_FOCUS_ITEMS) return;

    if (s_received_mask & ((uint32_t)1 << chunk_index)) return;
    s_received_mask |= (uint32_t)1 << chunk_index;

    UiItem parsed[MAX_FOCUS_ITEMS];
    int n = protocol_parse_items(iter, parsed, 1);
    if (n > 0 && chunk_index < MAX_FOCUS_ITEMS) {
        s_items[chunk_index] = parsed[0];
        if (chunk_index + 1 > s_count) s_count = chunk_index + 1;
    }

    if (s_timer) app_timer_cancel(s_timer);
    s_timer = app_timer_register(5000, chunk_timeout_handler, NULL);

    uint32_t expected_mask = ((uint32_t)1 << s_expected_chunks) - 1;
    if ((s_received_mask & expected_mask) == expected_mask) {
        s_loading = false;
        layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
        layer_set_hidden(menu_layer_get_layer(s_menu_layer), false);
        menu_layer_reload_data(s_menu_layer);
    }
}
