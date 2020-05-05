/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for UI functions without real UI flow.
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2ui.h"
#include "2ui_private.h"
#include "test_common.h"
#include "vboot_api.h"
#include "vboot_kernel.h"

/* Fixed value for ignoring some checks. */
#define MOCK_IGNORE 0xffffu

/* Mock screen index for testing screen utility functions. */
#define MOCK_NO_SCREEN 0xef0
#define MOCK_SCREEN_BASE 0xeff
#define MOCK_SCREEN_MENU 0xfff
#define MOCK_SCREEN_TARGET0 0xff0
#define MOCK_SCREEN_TARGET1 0xff1
#define MOCK_SCREEN_TARGET2 0xff2
#define MOCK_SCREEN_TARGET3 0xff3
#define MOCK_SCREEN_TARGET4 0xff4

/* Mock data */
struct display_call {
	const struct vb2_screen_info *screen;
	uint32_t locale_id;
	uint32_t selected_item;
	uint32_t disabled_item_mask;
};

static uint8_t workbuf[VB2_KERNEL_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_gbb_header gbb;

static int mock_shutdown_request;

static struct vb2_ui_context mock_ui_context;
static struct vb2_screen_state *mock_state;

static struct display_call mock_displayed[64];
static int mock_displayed_count;
static int mock_displayed_i;

static uint32_t mock_key[64];
static int mock_key_trusted[64];
static int mock_key_count;
static int mock_key_total;

/* Mocks for testing screen utility functions. */
const struct vb2_menu_item mock_empty_menu[] = {};
struct vb2_screen_info mock_screen_blank = {
	.id = VB2_SCREEN_BLANK,
	.name = "mock_screen_blank",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};
struct vb2_screen_info mock_screen_base =
{
	.id = MOCK_SCREEN_BASE,
	.name = "mock_screen_base: menuless screen",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};
struct vb2_menu_item mock_screen_menu_items[] =
{
	{
		.text = "option 0",
		.target = MOCK_SCREEN_TARGET0,
	},
	{
		.text = "option 1",
		.target = MOCK_SCREEN_TARGET1,
	},
	{
		.text = "option 2",
		.target = MOCK_SCREEN_TARGET2,
	},
	{
		.text = "option 3",
		.target = MOCK_SCREEN_TARGET3,
	},
	{
		.text = "option 4 (no target)",
	},
};
const struct vb2_screen_info mock_screen_menu =
{
	.id = MOCK_SCREEN_MENU,
	.name = "mock_screen_menu: screen with 5 options",
	.num_items = ARRAY_SIZE(mock_screen_menu_items),
	.items = mock_screen_menu_items,
};
const struct vb2_screen_info mock_screen_target0 =
{
	.id = MOCK_SCREEN_TARGET0,
	.name = "mock_screen_target0",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};
const struct vb2_screen_info mock_screen_target1 =
{
	.id = MOCK_SCREEN_TARGET1,
	.name = "mock_screen_target1",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};
const struct vb2_screen_info mock_screen_target2 =
{
	.id = MOCK_SCREEN_TARGET2,
	.name = "mock_screen_target2",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};
const struct vb2_screen_info mock_screen_target3 =
{
	.id = MOCK_SCREEN_TARGET3,
	.name = "mock_screen_target3",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};
const struct vb2_screen_info mock_screen_target4 =
{
	.id = MOCK_SCREEN_TARGET4,
	.name = "mock_screen_target4",
	.num_items = ARRAY_SIZE(mock_empty_menu),
	.items = mock_empty_menu,
};

/* Actions for tests */
static uint32_t global_action_called;
static vb2_error_t global_action_countdown(struct vb2_ui_context *ui)
{
	if (++global_action_called >= 10)
		return VB2_SUCCESS;
	return VB2_REQUEST_UI_CONTINUE;
}

static vb2_error_t global_action_change_screen(struct vb2_ui_context *ui)
{
	change_screen(ui, MOCK_SCREEN_BASE);
	if (++global_action_called >= 10)
		return VB2_SUCCESS;
	return VB2_REQUEST_UI_CONTINUE;
}

static void screen_state_eq(const struct vb2_screen_state *state,
			    enum vb2_screen screen,
			    uint32_t selected_item,
			    uint32_t disabled_item_mask)
{
	if (screen != MOCK_IGNORE) {
		if (state->screen == NULL)
			TEST_TRUE(0, "  state.screen does not exist");
		else
			TEST_EQ(state->screen->id, screen, "  state.screen");
	}
	if (selected_item != MOCK_IGNORE)
		TEST_EQ(state->selected_item,
			selected_item, "  state.selected_item");
	if (disabled_item_mask != MOCK_IGNORE)
		TEST_EQ(state->disabled_item_mask,
			disabled_item_mask, "  state.disabled_item_mask");
}

static void add_mock_key(uint32_t press, int trusted)
{
	if (mock_key_total >= ARRAY_SIZE(mock_key) ||
	    mock_key_total >= ARRAY_SIZE(mock_key_trusted)) {
		TEST_TRUE(0, "  mock_key ran out of entries!");
		return;
	}

	mock_key[mock_key_total] = press;
	mock_key_trusted[mock_key_total] = trusted;
	mock_key_total++;
}

static void add_mock_keypress(uint32_t press)
{
	add_mock_key(press, 0);
}

static void displayed_eq(const char *text,
			 enum vb2_screen screen,
			 uint32_t locale_id,
			 uint32_t selected_item,
			 uint32_t disabled_item_mask)
{
	char text_buf[256];

	if (mock_displayed_i >= mock_displayed_count) {
		sprintf(text_buf, "  missing screen %s", text);
		TEST_TRUE(0, text_buf);
		return;
	}

	if (screen != MOCK_IGNORE) {
		sprintf(text_buf, "  screen of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].screen->id, screen,
			text_buf);
	}
	if (locale_id != MOCK_IGNORE) {
		sprintf(text_buf, "  locale_id of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].locale_id, locale_id,
			text_buf);
	}
	if (selected_item != MOCK_IGNORE) {
		sprintf(text_buf, "  selected_item of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].selected_item,
			selected_item, text_buf);
	}
	if (disabled_item_mask != MOCK_IGNORE) {
		sprintf(text_buf, "  disabled_item_mask of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].disabled_item_mask,
			disabled_item_mask, text_buf);
	}
	mock_displayed_i++;
}

static void displayed_no_extra(void)
{
	if (mock_displayed_i == 0)
		TEST_EQ(mock_displayed_count, 0, "  no screen");
	else
		TEST_EQ(mock_displayed_count, mock_displayed_i,
			"  no extra screens");
}

/* Reset mock data (for use before each test) */
static void reset_common_data(void)
{
	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");

	memset(&gbb, 0, sizeof(gbb));

	vb2_nv_init(ctx);

	/* For shutdown_required */
	power_button = POWER_BUTTON_HELD_SINCE_BOOT;
	mock_shutdown_request = MOCK_IGNORE;

	/* For menu actions */
	mock_ui_context = (struct vb2_ui_context){
		.ctx = ctx,
		.root_screen = &mock_screen_blank,
		.state = (struct vb2_screen_state){
			.screen = &mock_screen_blank,
			.selected_item = 0,
			.disabled_item_mask = 0,
		},
		.locale_id = 0,
		.key = 0,

	};
	mock_state = &mock_ui_context.state;

	/* For vb2ex_display_ui */
	memset(mock_displayed, 0, sizeof(mock_displayed));
	mock_displayed_count = 0;
	mock_displayed_i = 0;

	/* For VbExKeyboardRead */
	memset(mock_key, 0, sizeof(mock_key));
	memset(mock_key_trusted, 0, sizeof(mock_key_trusted));
	mock_key_count = 0;
	mock_key_total = 0;

	/* For global actions */
	global_action_called = 0;
}

/* Mock functions */
struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &gbb;
}

uint32_t VbExIsShutdownRequested(void)
{
	if (mock_shutdown_request != MOCK_IGNORE)
		return mock_shutdown_request;

	return 0;
}

const struct vb2_screen_info *vb2_get_screen_info(enum vb2_screen screen)
{
	switch ((int)screen) {
	case VB2_SCREEN_BLANK:
		return &mock_screen_blank;
	case MOCK_SCREEN_BASE:
		return &mock_screen_base;
	case MOCK_SCREEN_MENU:
		return &mock_screen_menu;
	case MOCK_SCREEN_TARGET0:
		return &mock_screen_target0;
	case MOCK_SCREEN_TARGET1:
		return &mock_screen_target1;
	case MOCK_SCREEN_TARGET2:
		return &mock_screen_target2;
	case MOCK_SCREEN_TARGET3:
		return &mock_screen_target3;
	case MOCK_SCREEN_TARGET4:
		return &mock_screen_target4;
	default:
		return NULL;
	}
}

vb2_error_t vb2ex_display_ui(enum vb2_screen screen,
			     uint32_t locale_id,
			     uint32_t selected_item,
			     uint32_t disabled_item_mask)
{
	VB2_DEBUG("displayed %d: screen = %#x, locale_id = %u, "
		  "selected_item = %u, disabled_item_mask = %#x\n",
		  mock_displayed_count, screen, locale_id, selected_item,
		  disabled_item_mask);

	if (mock_displayed_count >= ARRAY_SIZE(mock_displayed)) {
		TEST_TRUE(0, "  mock vb2ex_display_ui ran out of entries!");
		return VB2_ERROR_MOCK;
	}

	mock_displayed[mock_displayed_count] = (struct display_call){
		.screen = vb2_get_screen_info(screen),
		.locale_id = locale_id,
		.selected_item = selected_item,
		.disabled_item_mask = disabled_item_mask,
	};
	mock_displayed_count++;

	return VB2_SUCCESS;
}

uint32_t VbExKeyboardRead(void)
{
	return VbExKeyboardReadWithFlags(NULL);
}

uint32_t VbExKeyboardReadWithFlags(uint32_t *key_flags)
{
	if (mock_key_count < mock_key_total) {
		if (key_flags != NULL) {
			if (mock_key_trusted[mock_key_count])
				*key_flags = VB_KEY_FLAG_TRUSTED_KEYBOARD;
			else
				*key_flags = 0;
		}
		return mock_key[mock_key_count++];
	}

	return 0;
}

/* Tests */
static void shutdown_required_tests(void)
{
	VB2_DEBUG("Testing shutdown_required...\n");

	/* Release, press, hold, and release */
	if (!DETACHABLE) {
		reset_common_data();
		mock_shutdown_request = 0;
		TEST_EQ(shutdown_required(ctx, 0), 0,
			"release, press, hold, and release");
		mock_shutdown_request = VB_SHUTDOWN_REQUEST_POWER_BUTTON;
		TEST_EQ(shutdown_required(ctx, 0), 0, "  press");
		TEST_EQ(shutdown_required(ctx, 0), 0, "  hold");
		mock_shutdown_request = 0;
		TEST_EQ(shutdown_required(ctx, 0), 1, "  release");
	}

	/* Press is ignored because we may held since boot */
	if (!DETACHABLE) {
		reset_common_data();
		mock_shutdown_request = VB_SHUTDOWN_REQUEST_POWER_BUTTON;
		TEST_EQ(shutdown_required(ctx, 0), 0, "press is ignored");
	}

	/* Power button short press from key */
	if (!DETACHABLE) {
		reset_common_data();
		mock_shutdown_request = 0;
		TEST_EQ(shutdown_required(ctx, VB_BUTTON_POWER_SHORT_PRESS), 1,
			"power button short press");
	}

	/* Lid closure = shutdown request anyway */
	reset_common_data();
	mock_shutdown_request = VB_SHUTDOWN_REQUEST_LID_CLOSED;
	TEST_EQ(shutdown_required(ctx, 0), 1, "lid closure");
	TEST_EQ(shutdown_required(ctx, 'A'), 1, "  lidsw + random key");

	/* Lid ignored by GBB flags */
	reset_common_data();
	gbb.flags |= VB2_GBB_FLAG_DISABLE_LID_SHUTDOWN;
	mock_shutdown_request = VB_SHUTDOWN_REQUEST_LID_CLOSED;
	TEST_EQ(shutdown_required(ctx, 0), 0, "lid ignored");
	if (!DETACHABLE) {  /* Power button works for non DETACHABLE */
		mock_shutdown_request = VB_SHUTDOWN_REQUEST_LID_CLOSED |
					VB_SHUTDOWN_REQUEST_POWER_BUTTON;
		TEST_EQ(shutdown_required(ctx, 0), 0, "  lidsw + pwdsw");
		mock_shutdown_request = 0;
		TEST_EQ(shutdown_required(ctx, 0), 1, "  pwdsw release");
	}

	/* Lid ignored; power button short pressed */
	if (!DETACHABLE) {
		reset_common_data();
		gbb.flags |= VB2_GBB_FLAG_DISABLE_LID_SHUTDOWN;
		mock_shutdown_request = VB_SHUTDOWN_REQUEST_LID_CLOSED;
		TEST_EQ(shutdown_required(ctx, VB_BUTTON_POWER_SHORT_PRESS), 1,
			"lid ignored; power button short pressed");
	}

	/* DETACHABLE ignore power button */
	if (DETACHABLE) {
		/* Flag pwdsw */
		reset_common_data();
		mock_shutdown_request = VB_SHUTDOWN_REQUEST_POWER_BUTTON;
		TEST_EQ(shutdown_required(ctx, 0), 0,
			"DETACHABLE: ignore pwdsw");
		mock_shutdown_request = 0;
		TEST_EQ(shutdown_required(ctx, 0), 0,
			"  ignore on release");

		/* Power button short press */
		reset_common_data();
		mock_shutdown_request = 0;
		TEST_EQ(shutdown_required(
		    ctx, VB_BUTTON_POWER_SHORT_PRESS), 0,
		    "DETACHABLE: ignore power button short press");
	}

	VB2_DEBUG("...done.\n");
}

static void menu_action_tests(void)
{
	int i, target_id;
	char test_name[256];

	VB2_DEBUG("Testing menu actions...\n");

	/* Valid menu_up_action */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_ui_context.key = VB_KEY_UP;
	TEST_EQ(menu_up_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"valid menu_up_action");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 1, MOCK_IGNORE);

	/* Valid menu_up_action with mask */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x0a;  /* 0b01010 */
	mock_ui_context.key = VB_KEY_UP;
	TEST_EQ(menu_up_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"valid menu_up_action with mask");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 0, MOCK_IGNORE);

	/* Invalid menu_up_action (blocked) */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 0;
	mock_ui_context.key = VB_KEY_UP;
	TEST_EQ(menu_up_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"invalid menu_up_action (blocked)");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 0, MOCK_IGNORE);

	/* Invalid menu_up_action (blocked by mask) */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x0b;  /* 0b01011 */
	mock_ui_context.key = VB_KEY_UP;
	TEST_EQ(menu_up_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"invalid menu_up_action (blocked by mask)");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 2, MOCK_IGNORE);

	/* Ignore volume-up when not DETACHABLE */
	if (!DETACHABLE) {
		reset_common_data();
		mock_state->screen = &mock_screen_menu;
		mock_state->selected_item = 2;
		mock_ui_context.key = VB_BUTTON_VOL_UP_SHORT_PRESS;
		TEST_EQ(menu_up_action(&mock_ui_context),
			VB2_REQUEST_UI_CONTINUE,
			"ignore volume-up when not DETACHABLE");
		screen_state_eq(mock_state, MOCK_SCREEN_MENU, 2, MOCK_IGNORE);
	}

	/* Valid menu_down_action */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_ui_context.key = VB_KEY_DOWN;
	TEST_EQ(menu_down_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"valid menu_down_action");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 3, MOCK_IGNORE);

	/* Valid menu_down_action with mask */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x0a;  /* 0b01010 */
	mock_ui_context.key = VB_KEY_DOWN;
	TEST_EQ(menu_down_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"valid menu_down_action with mask");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 4, MOCK_IGNORE);

	/* Invalid menu_down_action (blocked) */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 4;
	mock_ui_context.key = VB_KEY_DOWN;
	TEST_EQ(menu_down_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"invalid menu_down_action (blocked)");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 4, MOCK_IGNORE);

	/* Invalid menu_down_action (blocked by mask) */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x1a;  /* 0b11010 */
	mock_ui_context.key = VB_KEY_DOWN;
	TEST_EQ(menu_down_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"invalid menu_down_action (blocked by mask)");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 2, MOCK_IGNORE);

	/* Ignore volume-down when not DETACHABLE */
	if (!DETACHABLE) {
		reset_common_data();
		mock_state->screen = &mock_screen_menu;
		mock_state->selected_item = 2;
		mock_ui_context.key = VB_BUTTON_VOL_DOWN_SHORT_PRESS;
		TEST_EQ(menu_down_action(&mock_ui_context),
			VB2_REQUEST_UI_CONTINUE,
			"ignore volume-down when not DETACHABLE");
		screen_state_eq(mock_state, MOCK_SCREEN_MENU, 2, MOCK_IGNORE);
	}

	/* menu_select_action with no item screen */
	reset_common_data();
	mock_state->screen = &mock_screen_base;
	mock_ui_context.key = VB_KEY_ENTER;
	TEST_EQ(menu_select_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"menu_select_action with no item screen");
	screen_state_eq(mock_state, MOCK_SCREEN_BASE, 0, MOCK_IGNORE);

	/* Try to select target 0..3 */
	for (i = 0; i <= 3; i++) {
		sprintf(test_name, "select target %d", i);
		target_id = MOCK_SCREEN_TARGET0 + i;
		reset_common_data();
		mock_state->screen = &mock_screen_menu;
		mock_state->selected_item = i;
		mock_ui_context.key = VB_KEY_ENTER;
		TEST_EQ(menu_select_action(&mock_ui_context),
			VB2_REQUEST_UI_CONTINUE, test_name);
		screen_state_eq(mock_state, target_id, 0, MOCK_IGNORE);
	}

	/* Try to select no target item */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 4;
	mock_ui_context.key = VB_KEY_ENTER;
	TEST_EQ(menu_select_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"select no target");
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 4, MOCK_IGNORE);

	/* Ignore power button short press when not DETACHABLE */
	if (!DETACHABLE) {
		reset_common_data();
		mock_state->screen = &mock_screen_menu;
		mock_state->selected_item = 1;
		mock_ui_context.key = VB_BUTTON_POWER_SHORT_PRESS;
		TEST_EQ(menu_select_action(&mock_ui_context),
			VB2_REQUEST_UI_CONTINUE,
			"ignore power button short press when not DETACHABLE");
		screen_state_eq(mock_state, MOCK_SCREEN_MENU, 1, MOCK_IGNORE);
	}

	/* menu_back_action */
	reset_common_data();
	mock_ui_context.key = VB_KEY_ESC;
	TEST_EQ(menu_back_action(&mock_ui_context), VB2_REQUEST_UI_CONTINUE,
		"menu_back_action");
	screen_state_eq(mock_state, VB2_SCREEN_BLANK, 0, MOCK_IGNORE);

	VB2_DEBUG("...done.\n");
}

static void change_screen_tests(void)
{
	VB2_DEBUG("Testing change_screen...\n");

	/* Changing screen will clear screen state */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x10;
	VB2_DEBUG("change_screen will clear screen state\n");
	change_screen(&mock_ui_context, MOCK_SCREEN_BASE);
	screen_state_eq(mock_state, MOCK_SCREEN_BASE, 0, 0);

	/* Change to screen which does not exist */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	VB2_DEBUG("change to screen which does not exist\n");
	change_screen(&mock_ui_context, MOCK_NO_SCREEN);
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, MOCK_IGNORE, MOCK_IGNORE);

	VB2_DEBUG("...done.\n");
}

static void validate_selection_tests(void)
{
	VB2_DEBUG("Testing validate_selection...");

	/* No item */
	reset_common_data();
	mock_state->screen = &mock_screen_base;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x10;
	VB2_DEBUG("no item (fix selected_item)\n");
	validate_selection(mock_state);
	screen_state_eq(mock_state, MOCK_SCREEN_BASE, 0, MOCK_IGNORE);

	/* Valid selected_item */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x13;  /* 0b10011 */
	VB2_DEBUG("valid selected_item\n");
	validate_selection(mock_state);
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 2, MOCK_IGNORE);

	/* selected_item too large */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 5;
	mock_state->disabled_item_mask = 0x15;  /* 0b10101 */
	VB2_DEBUG("selected_item too large\n");
	validate_selection(mock_state);
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 1, MOCK_IGNORE);

	/* Select a disabled item */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 4;
	mock_state->disabled_item_mask = 0x17;  /* 0b10111 */
	VB2_DEBUG("select a disabled item\n");
	validate_selection(mock_state);
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 3, MOCK_IGNORE);

	/* No available item */
	reset_common_data();
	mock_state->screen = &mock_screen_menu;
	mock_state->selected_item = 2;
	mock_state->disabled_item_mask = 0x1f;  /* 0b11111 */
	VB2_DEBUG("no available item\n");
	validate_selection(mock_state);
	screen_state_eq(mock_state, MOCK_SCREEN_MENU, 0, MOCK_IGNORE);

	VB2_DEBUG("...done.\n");
}

static void ui_loop_tests(void)
{
	VB2_DEBUG("Testing ui_loop...\n");

	/* Die if no root screen */
	reset_common_data();
	TEST_ABORT(ui_loop(ctx, MOCK_NO_SCREEN, NULL),
		   "die if no root screen");
	displayed_no_extra();

	/* Shutdown if requested */
	reset_common_data();
	mock_shutdown_request = VB_SHUTDOWN_REQUEST_OTHER;
	TEST_EQ(ui_loop(ctx, MOCK_SCREEN_BASE, NULL),
		VB2_REQUEST_SHUTDOWN, "shutdown if requested");
	displayed_eq("mock_screen_base", MOCK_SCREEN_BASE, MOCK_IGNORE,
		     MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* Global action */
	reset_common_data();
	TEST_EQ(ui_loop(ctx, VB2_SCREEN_BLANK, global_action_countdown),
		VB2_SUCCESS, "global action");
	TEST_EQ(global_action_called, 10, "  global action called");

	/* Global action can change screen */
	reset_common_data();
	TEST_EQ(ui_loop(ctx, VB2_SCREEN_BLANK, global_action_change_screen),
		VB2_SUCCESS, "global action can change screen");
	TEST_EQ(global_action_called, 10, "  global action called");
	displayed_eq("pass", MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE,
		     MOCK_IGNORE);
	displayed_eq("change to mock_screen_base", MOCK_IGNORE, MOCK_IGNORE,
		     MOCK_IGNORE, MOCK_IGNORE);

	/* KEY_UP, KEY_DOWN, and KEY_ENTER navigation */
	reset_common_data();
	add_mock_keypress(VB_KEY_UP);  /* (blocked) */
	add_mock_keypress(VB_KEY_DOWN);
	add_mock_keypress(VB_KEY_DOWN);
	add_mock_keypress(VB_KEY_DOWN);
	add_mock_keypress(VB_KEY_DOWN);
	add_mock_keypress(VB_KEY_DOWN);  /* (blocked) */
	add_mock_keypress(VB_KEY_UP);
	add_mock_keypress(VB_KEY_ENTER);
	TEST_EQ(ui_loop(ctx, MOCK_SCREEN_MENU, global_action_countdown),
		VB2_SUCCESS, "KEY_UP, KEY_DOWN, and KEY_ENTER");
	displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE, 0,
		     MOCK_IGNORE);
	displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE, 1,
		     MOCK_IGNORE);
	displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE, 2,
		     MOCK_IGNORE);
	displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE, 3,
		     MOCK_IGNORE);
	displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE, 4,
		     MOCK_IGNORE);
	displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE, 3,
		     MOCK_IGNORE);
	displayed_eq("mock_screen_target_3", MOCK_SCREEN_TARGET3, MOCK_IGNORE,
		     MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* For DETACHABLE */
	if (DETACHABLE) {
		reset_common_data();
		add_mock_keypress(VB_BUTTON_VOL_UP_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_VOL_DOWN_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_VOL_DOWN_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_VOL_DOWN_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_VOL_DOWN_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_VOL_DOWN_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_VOL_UP_SHORT_PRESS);
		add_mock_keypress(VB_BUTTON_POWER_SHORT_PRESS);
		TEST_EQ(ui_loop(ctx, MOCK_SCREEN_MENU, global_action_countdown),
			VB2_SUCCESS, "DETACHABLE");
		displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE,
			     0, MOCK_IGNORE);
		displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE,
			     1, MOCK_IGNORE);
		displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE,
			     2, MOCK_IGNORE);
		displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE,
			     3, MOCK_IGNORE);
		displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE,
			     4, MOCK_IGNORE);
		displayed_eq("mock_screen_menu", MOCK_SCREEN_MENU, MOCK_IGNORE,
			     3, MOCK_IGNORE);
		displayed_eq("mock_screen_target_3", MOCK_SCREEN_TARGET3,
			     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
		displayed_no_extra();
	}

	VB2_DEBUG("...done.\n");
}

int main(void)
{
	shutdown_required_tests();
	menu_action_tests();
	change_screen_tests();
	validate_selection_tests();
	ui_loop_tests();

	return gTestSuccess ? 0 : 255;
}
