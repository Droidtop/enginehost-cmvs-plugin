/*
 * The scene camera: what turns a staged object's place in the WORLD into a
 * place on the screen.
 *
 * A CMVS scene is not flat. A draw item carries either a screen position
 * (command 0x045) or a world position (commands 0x072, 0x073, 0x074), and
 * command 0x042 / 0x043 says which of the two the compositor must use
 * (0x0041beb0 writes 2 into the item's first field, 0x0041bec0 writes 2 or 3).
 * The compositor at 0x00435da0 sorts the items into buckets by that field and
 * sends the world ones through a camera: 0x00435f7e picks camera 0 for kind 2
 * (0x00418ab0 reads world+8) and camera 1 for kind 3 (0x00416ed0, world+0xc),
 * and 0x0041d804 hands the item's world triple, its own reference depth and
 * its scale to the camera's projection at 0x00443da0.
 *
 * This is why ChronoClock's rooftop background had no position. snky01.ps3
 * loads bg150a.pb3 into object 28, gives it its own centre (910, 512) as the
 * anchor and never calls 0x045, because the anchor is not meant to be measured
 * from a screen position at all: it is measured from where the camera puts the
 * object's world point, which for an object standing on the camera's own axis
 * is the middle of the screen.
 *
 * The camera itself is built at 0x004437c0 and configured by the 0x05x family:
 *   0x058 -> 0x00443b00  the reference depth and the world extent seen there
 *   0x059 -> 0x00443b20  the screen size, and with it the principal point
 *   0x05a -> 0x00443b50  where the camera stands
 *   0x05b -> 0x00443be0  its spin
 *   0x05c -> 0x00443c20  the lift applied to an item away from its own depth
 *   0x05f -> 0x004532a0  which projection it is
 *   0x067 -> 0x00443bc0  the aspect divisors
 *   0x062 -> 0x00443b50  where camera 0 stands, without naming it
 */
#ifndef CMVS_CAMERA_H
#define CMVS_CAMERA_H

/* world+8 onwards: the compositor only ever reaches the first two. */
#define CMVS_CAMERAS 8

typedef struct {
    int kind;                    /* +0x00, command 0x05f; 3 is the one below */
    float x, y, z;               /* +0x04 +0x08 +0x0c, commands 0x05a / 0x062 */
    float reference_depth;       /* +0x18, command 0x058: where the extent holds */
    float view_width;            /* +0x1c: world units across at that depth */
    float view_height;           /* +0x20: and down */
    float screen_width;          /* +0x24, command 0x059 */
    float screen_height;         /* +0x28 */
    float lift;                  /* +0x2c, command 0x05c */
    float roll;                  /* +0x34: zero in every script read so far */
    float spin;                  /* +0x38, command 0x05b */
    float centre_x, centre_y;    /* +0x3c +0x40: half the screen, from 0x059 */
    float aspect_x, aspect_y;    /* +0x44 +0x48, command 0x067 */
    int mode;                    /* +0x9c, command 0x066: carried, not read */
    int alternate;               /* +0x94, command 0x05e: carried, not read */
} cmvs_camera;

/* What an item hands the camera: the packed block 0x0041d72d builds out of the
 * draw item before it calls the projection. */
typedef struct {
    float x, y, z;               /* item +0x2c +0x30 +0x34, commands 0x072-4 */
    float lift;                  /* item +0x40, command 0x071 */
    float plane;                 /* item +0x3c, command 0x070: its own depth */
    float scale_x, scale_y;      /* item +0x48 +0x4c, 1 out of the constructor */
} cmvs_placement;

typedef struct {
    float x, y;                  /* where the item's anchor lands on screen */
    float scale_x, scale_y;
    int alpha;                   /* the distance fade, 255 for anything near */
} cmvs_projection;

void cmvs_camera_init(cmvs_camera *c);

/*
 * Projects one placement. Answers 0 when the camera does not draw it at all -
 * either it is nearer than the front plane, or this camera is one of the two
 * projections that have not been read yet (see camera.c).
 */
int cmvs_camera_project(const cmvs_camera *c, const cmvs_placement *p,
                        cmvs_projection *out);

#endif
