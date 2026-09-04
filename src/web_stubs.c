/*
 * web_stubs.c - web build shims for desktop-only engine surfaces.
 *
 * The canvas backend has no 3D pipeline and the web build drops the Box2D
 * physics nodes, but the engine view's shared C (native_engine.c) still
 * references those APIs. On the web they resolve here as inert no-ops:
 * 3D meshes come back empty and physics nodes behave like plain Node2Ds,
 * which keeps the rest of the scene engine usable in the browser.
 */
#include "kryon.h"
#include "scene_tree.h"
#include "node2d_props.h"

#include <stdlib.h>

/* Resolve kry_screenshot.c's weak GL/raylib seams with real no-ops on the
 * canvas link so their address checks and armed-path calls stay in-bounds.
 * The canvas backend implements KryonRaylibBackend_EndDrawing itself. */
void rlDrawRenderBatchActive(void) { }
void glReadPixels(int x, int y, int w, int h, unsigned format, unsigned type,
                  void *data)
{
    (void)x; (void)y; (void)w; (void)h; (void)format; (void)type; (void)data;
}

/* --- raylib-compat 3D ------------------------------------------------ */

Mesh GenMeshCube(float w, float h, float d)
{
    Mesh mesh = {0};
    (void)w; (void)h; (void)d;
    return mesh;
}

Mesh GenMeshSphere(float radius, int rings, int slices)
{
    Mesh mesh = {0};
    (void)radius; (void)rings; (void)slices;
    return mesh;
}

Mesh GenMeshPlane(float w, float h, int resX, int resZ)
{
    Mesh mesh = {0};
    (void)w; (void)h; (void)resX; (void)resZ;
    return mesh;
}

Mesh GenMeshCone(float radius, float height, int slices)
{
    Mesh mesh = {0};
    (void)radius; (void)height; (void)slices;
    return mesh;
}

Mesh GenMeshCylinder(float radius, float height, int slices)
{
    Mesh mesh = {0};
    (void)radius; (void)height; (void)slices;
    return mesh;
}

Mesh GenMeshTorus(float radius, float size, int radSeg, int sideSeg)
{
    Mesh mesh = {0};
    (void)radius; (void)size; (void)radSeg; (void)sideSeg;
    return mesh;
}

Model LoadModel(const char *filename)
{
    Model model = {0};
    (void)filename;
    return model;
}

void UnloadModel(Model model)
{
    (void)model;
}

Model LoadModelFromMesh(Mesh mesh)
{
    Model model = {0};
    (void)mesh;
    return model;
}

void DrawModel(Model model, Vector3 position, float scale, Color tint)
{
    (void)model; (void)position; (void)scale; (void)tint;
}

void BeginMode3D(Camera3D camera)
{
    (void)camera;
}

void EndMode3D(void)
{
}

void DrawGrid(int slices, float spacing)
{
    (void)slices; (void)spacing;
}

/* --- Box2D physics nodes --------------------------------------------- */

int ScenePhysicsCreate(Scene *scene, float gravity_x, float gravity_y)
{
    (void)scene; (void)gravity_x; (void)gravity_y;
    return 0;
}

void ScenePhysicsDestroy(Scene *scene)
{
    (void)scene;
}

void KryBody2DGetVelocity(Scene *scene, NodeId node, float *vx, float *vy)
{
    (void)scene; (void)node;
    if(vx != NULL)
        *vx = 0.0f;
    if(vy != NULL)
        *vy = 0.0f;
}

void KryBody2DSetVelocity(Scene *scene, NodeId node, float vx, float vy)
{
    (void)scene; (void)node; (void)vx; (void)vy;
}

Body2DProps *KryBody2DPropsAlloc(KryBody2DType type)
{
    Body2DProps *props = (Body2DProps *)calloc(1, sizeof *props);

    if(props != NULL)
        props->body_type = type;
    return props;
}

CollisionShape2DProps *KryCollisionShape2DPropsAlloc(KryShape2DKind kind,
                                                     float w, float h)
{
    CollisionShape2DProps *props =
        (CollisionShape2DProps *)calloc(1, sizeof *props);

    if(props != NULL) {
        props->shape_kind = kind;
        props->size.x = w;
        props->size.y = h;
    }
    return props;
}

Area2DProps *KryArea2DPropsAlloc(void)
{
    return (Area2DProps *)calloc(1, sizeof(Area2DProps));
}
