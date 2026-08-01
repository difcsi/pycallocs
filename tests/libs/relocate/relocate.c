// Fixture for the "relocate" test: prove a pycallocs proxy survives an Alaska
// backing relocation.
//
// Under the full Alaska handle transform, malloc() hands back a stable *handle*
// whose backing memory the allocator/GC may move. hrealloc() forces such a move
// while keeping the handle (and its liballocs lifetime-policy insert) intact, so a
// pycallocs proxy -- which indexes by handle and is kept alive by a liballocs
// lifetime policy -- must keep reading and writing correctly across the move.
//
// The grow target stays in the handle regime (<= page_size/2, Alaska's huge-object
// threshold) so hrealloc takes the handle->handle path: the SAME handle is kept and
// only the backing is relocated. (A huge grow would instead free the handle and
// return a different pointer -- not an in-place handle move.)
//
// This fixture HARD-depends on the Alaska runtime: hrealloc()/alaska_translate()
// are plain (non-weak) externs, so if Alaska is absent the .so fails to load. That
// is intentional -- CMake only builds/registers it under Alaska. It includes NO
// system headers: allocscc's Alaska (clang) backend exposes native _Float* types
// CIL cannot parse, so everything needed is forward-declared via __SIZE_TYPE__.

extern void *malloc(__SIZE_TYPE__ sz);
extern void *hrealloc(void *handle, __SIZE_TYPE__ sz);
extern void *alaska_translate(void *ptr);

struct point { int x; int y; };

struct point *make_point(int x, int y)
{
    struct point *p = malloc(sizeof *p);
    p->x = x;
    p->y = y;
    return p;
}

// The raw (translated) backing address of a handle, as an integer. Lets the test
// observe that the backing genuinely moved.
unsigned long backing_addr(struct point *p)
{
    return (unsigned long) alaska_translate(p);
}

// Grow the backing to a different size class so the allocator relocates it, while
// staying under the huge-object threshold so the handle is preserved. Returns void
// on purpose: the handle is unchanged, and the proxy's liballocs lifetime policy
// (whose insert hrealloc relocates with the object) keeps the object alive, so the
// caller needs no update -- the proxy stays oblivious to the move.
void relocate_point(struct point *p)
{
    hrealloc(p, 1024);
}
