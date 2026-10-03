#include "../src/renderer.h"


static void
assert_layout(const CaptureRendererLayout *layout,
              gint x, gint y, gint width, gint height,
              gdouble u0, gdouble v0, gdouble u1, gdouble v1)
{
    g_assert_cmpint(layout->viewport.x, ==, x);
    g_assert_cmpint(layout->viewport.y, ==, y);
    g_assert_cmpint(layout->viewport.width, ==, width);
    g_assert_cmpint(layout->viewport.height, ==, height);
    g_assert_cmpfloat_with_epsilon(layout->u0, u0, 0.000001);
    g_assert_cmpfloat_with_epsilon(layout->v0, v0, 0.000001);
    g_assert_cmpfloat_with_epsilon(layout->u1, u1, 0.000001);
    g_assert_cmpfloat_with_epsilon(layout->v1, v1, 0.000001);
}

static void
test_fit_identity(void)
{
    CaptureRendererLayout layout;
    g_assert_true(capture_renderer_compute_layout(
        1280, 720, 1, 1, 1280, 720, CAPTURE_RENDERER_FIT, &layout));
    assert_layout(&layout, 0, 0, 1280, 720, 0.0, 0.0, 1.0, 1.0);
}

static void
test_fit_wide_and_tall_allocations(void)
{
    CaptureRendererLayout layout;
    g_assert_true(capture_renderer_compute_layout(
        1280, 720, 1, 1, 1600, 600, CAPTURE_RENDERER_FIT, &layout));
    assert_layout(&layout, 266, 0, 1067, 600, 0.0, 0.0, 1.0, 1.0);

    g_assert_true(capture_renderer_compute_layout(
        1280, 720, 1, 1, 600, 1000, CAPTURE_RENDERER_FIT, &layout));
    assert_layout(&layout, 0, 331, 600, 338, 0.0, 0.0, 1.0, 1.0);
}

static void
test_fill_wide_and_tall_allocations(void)
{
    CaptureRendererLayout layout;
    g_assert_true(capture_renderer_compute_layout(
        1280, 720, 1, 1, 1600, 600, CAPTURE_RENDERER_FILL, &layout));
    assert_layout(&layout, 0, 0, 1600, 600, 0.0, 1.0 / 6.0, 1.0, 5.0 / 6.0);

    g_assert_true(capture_renderer_compute_layout(
        1280, 720, 1, 1, 600, 1000, CAPTURE_RENDERER_FILL, &layout));
    assert_layout(&layout, 0, 0, 600, 1000, 0.33125, 0.0, 0.66875, 1.0);
}

static void
test_non_square_pixel_aspect(void)
{
    CaptureRendererLayout layout;
    g_assert_true(capture_renderer_compute_layout(
        720, 576, 16, 15, 1280, 720, CAPTURE_RENDERER_FIT, &layout));
    assert_layout(&layout, 160, 0, 960, 720, 0.0, 0.0, 1.0, 1.0);
}

static void
assert_invalid_layout(guint source_width, guint source_height,
                      guint par_num, guint par_den,
                      guint area_width, guint area_height,
                      CaptureRendererScaleMode mode)
{
    CaptureRendererLayout layout = {
        .viewport = { 1, 2, 3, 4 },
        .u0 = 0.1,
        .v0 = 0.2,
        .u1 = 0.3,
        .v1 = 0.4,
    };
    g_assert_false(capture_renderer_compute_layout(
        source_width, source_height, par_num, par_den,
        area_width, area_height, mode, &layout));
    assert_layout(&layout, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0);
}

static void
test_invalid_and_zero_inputs(void)
{
    assert_invalid_layout(0, 720, 1, 1, 1280, 720, CAPTURE_RENDERER_FIT);
    assert_invalid_layout(1280, 0, 1, 1, 1280, 720, CAPTURE_RENDERER_FIT);
    assert_invalid_layout(1280, 720, 0, 1, 1280, 720, CAPTURE_RENDERER_FIT);
    assert_invalid_layout(1280, 720, 1, 0, 1280, 720, CAPTURE_RENDERER_FIT);
    assert_invalid_layout(1280, 720, 1, 1, 0, 720, CAPTURE_RENDERER_FIT);
    assert_invalid_layout(1280, 720, 1, 1, 1280, 0, CAPTURE_RENDERER_FIT);
    assert_invalid_layout(1280, 720, 1, 1, 1280, 720,
                         (CaptureRendererScaleMode)99);
    assert_invalid_layout(1280, 720, 1, 1, (guint)G_MAXINT + 1u, 720,
                         CAPTURE_RENDERER_FILL);
    g_assert_false(capture_renderer_compute_layout(
        1280, 720, 1, 1, 1280, 720, CAPTURE_RENDERER_FIT, NULL));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/renderer/layout/fit-identity", test_fit_identity);
    g_test_add_func("/renderer/layout/fit-allocation-aspect",
                    test_fit_wide_and_tall_allocations);
    g_test_add_func("/renderer/layout/fill-centered-crop",
                    test_fill_wide_and_tall_allocations);
    g_test_add_func("/renderer/layout/non-square-pixel-aspect",
                    test_non_square_pixel_aspect);
    g_test_add_func("/renderer/layout/invalid-inputs", test_invalid_and_zero_inputs);
    return g_test_run();
}
