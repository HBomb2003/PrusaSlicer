// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoSlaSupports.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <GL/glew.h>

#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/stattext.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_ObjectSettings.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PresetBundle.hpp"
#include "libslic3r/Tesselate.hpp"


namespace Slic3r {
namespace GUI {

#if ENABLE_SVG_ICONS
GLGizmoSlaSupports::GLGizmoSlaSupports(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
#else
GLGizmoSlaSupports::GLGizmoSlaSupports(GLCanvas3D& parent, unsigned int sprite_id)
    : GLGizmoBase(parent, sprite_id)
#endif // ENABLE_SVG_ICONS
    , m_quadric(nullptr)
    , m_its(nullptr)
{
    m_quadric = ::gluNewQuadric();
    if (m_quadric != nullptr)
        // using GLU_FILL does not work when the instance's transformation
        // contains mirroring (normals are reverted)
        ::gluQuadricDrawStyle(m_quadric, GLU_FILL);
}

GLGizmoSlaSupports::~GLGizmoSlaSupports()
{
    if (m_quadric != nullptr)
        ::gluDeleteQuadric(m_quadric);
}

bool GLGizmoSlaSupports::on_init()
{
    m_shortcut_key = WXK_CONTROL_L;

    m_desc["head_diameter"]    = _(L("Head diameter")) + ": ";
    m_desc["lock_supports"]    = _(L("Lock supports under new islands"));
    m_desc["remove_selected"]  = _(L("Remove selected points"));
    m_desc["remove_all"]       = _(L("Remove all points"));
    m_desc["apply_changes"]    = _(L("Apply changes"));
    m_desc["discard_changes"]  = _(L("Discard changes"));
    m_desc["minimal_distance"] = _(L("Minimal points distance")) + ": ";
    m_desc["points_density"]   = _(L("Support points density")) + ": ";
    m_desc["auto_generate"]    = _(L("Auto-generate points"));
    m_desc["manual_editing"]   = _(L("Manual editing"));
    m_desc["clipping_of_view"] = _(L("Clipping of view"))+ ": ";
    m_desc["reset_direction"]  = _(L("Reset direction"));

    return true;
}

void GLGizmoSlaSupports::set_sla_support_data(ModelObject* model_object, const Selection& selection)
{
    if (selection.is_empty()) {
        m_model_object = nullptr;
        return;
    }

    if (m_model_object != model_object)
        m_print_object_idx = -1;

    m_model_object = model_object;
    m_active_instance = selection.get_instance_idx();

    if (model_object && selection.is_from_single_instance())
    {
        // Cache the bb - it's needed for dealing with the clipping plane quite often
        // It could be done inside update_mesh but one has to account for scaling of the instance.
        //FIXME calling ModelObject::instance_bounding_box() is expensive!
        m_active_instance_bb_radius = m_model_object->instance_bounding_box(m_active_instance).radius();

        if (is_mesh_update_necessary()) {
            update_mesh();
            editing_mode_reload_cache();
        }

        if (m_editing_mode_cache.empty() && m_model_object->sla_points_status != sla::PointsStatus::UserModified)
            get_data_from_backend();

        if (m_state == On) {
            m_parent.toggle_model_objects_visibility(false);
            m_parent.toggle_model_objects_visibility(true, m_model_object, m_active_instance);
        }
        else
            m_parent.toggle_model_objects_visibility(true, nullptr, -1);
    }
}

void GLGizmoSlaSupports::on_render(const Selection& selection) const
{
    // If current m_model_object does not match selection, ask GLCanvas3D to turn us off
    if (m_state == On
     && (m_model_object != selection.get_model()->objects[selection.get_object_idx()]
      || m_active_instance != selection.get_instance_idx())) {
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_RESETGIZMOS));
        return;
    }

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    m_z_shift = selection.get_volume(*selection.get_volume_idxs().begin())->get_sla_shift_z();

    if (m_quadric != nullptr && selection.is_from_single_instance())
        render_points(selection, false);

    m_selection_rectangle.render(m_parent);
    render_clipping_plane(selection);

    glsafe(::glDisable(GL_BLEND));
}



void GLGizmoSlaSupports::render_clipping_plane(const Selection& selection) const
{
    if (m_clipping_plane_distance == 0.f)
        return;

    if (m_clipping_plane_normal == Vec3d::Zero())
        reset_clipping_plane_normal();

    const Vec3d& direction_to_camera = m_clipping_plane_normal;

    // First cache instance transformation to be used later.
    const GLVolume* vol = selection.get_volume(*selection.get_volume_idxs().begin());
    Transform3f instance_matrix = vol->get_instance_transformation().get_matrix().cast<float>();
    Transform3f instance_matrix_no_translation_no_scaling = vol->get_instance_transformation().get_matrix(true,false,true).cast<float>();
    Vec3f scaling = vol->get_instance_scaling_factor().cast<float>();
    Vec3d instance_offset = vol->get_instance_offset();

    // Calculate distance from mesh origin to the clipping plane (in mesh coordinates).
    Vec3f up_noscale = instance_matrix_no_translation_no_scaling.inverse() * direction_to_camera.cast<float>();
    Vec3f up = Vec3f(up_noscale(0)*scaling(0), up_noscale(1)*scaling(1), up_noscale(2)*scaling(2));
    float height_mesh = (m_active_instance_bb_radius - m_clipping_plane_distance * 2*m_active_instance_bb_radius) * (up_noscale.norm()/up.norm());

    // Get transformation of the supports and calculate how far from its origin the clipping plane is.
    Transform3d supports_trafo = Transform3d::Identity();
    supports_trafo = supports_trafo.rotate(Eigen::AngleAxisd(vol->get_instance_rotation()(2), Vec3d::UnitZ()));
    Vec3f up_supports = (supports_trafo.inverse() * direction_to_camera).cast<float>();
    supports_trafo = supports_trafo.pretranslate(Vec3d(instance_offset(0), instance_offset(1), vol->get_sla_shift_z()));
    // Instance and supports origin do not coincide, so the following is quite messy:
    float height_supports = height_mesh * (up.norm() / up_supports.norm()) + instance_offset(2) * (direction_to_camera(2) / direction_to_camera.norm());

    // In case either of these was recently changed, the cached triangulated ExPolygons are invalid now.
    // We are gonna recalculate them both for the object and for the support structures.
    if (m_clipping_plane_distance != m_old_clipping_plane_distance
     || m_old_clipping_plane_normal != direction_to_camera) {

        m_old_clipping_plane_normal = direction_to_camera;
        m_old_clipping_plane_distance = m_clipping_plane_distance;

        // Now initialize the TMS for the object, perform the cut and save the result.
        if (! m_tms) {
            m_tms.reset(new TriangleMeshSlicer);
            m_tms->init(m_mesh, [](){});
        }
        std::vector<ExPolygons> list_of_expolys;
        m_tms->set_up_direction(up);
        m_tms->slice(std::vector<float>{height_mesh}, 0.f, &list_of_expolys, [](){});
        m_triangles = triangulate_expolygons_2f(list_of_expolys[0]);



        // Next, ask the backend if supports are already calculated. If so, we are gonna cut them too.
        // First we need a pointer to the respective SLAPrintObject. The index into objects vector is
        // cached so we don't have todo it on each render. We only search for the po if needed:
        if (m_print_object_idx < 0 || (int)m_parent.sla_print()->objects().size() != m_print_objects_count) {
            m_print_objects_count = m_parent.sla_print()->objects().size();
            m_print_object_idx = -1;
            for (const SLAPrintObject* po : m_parent.sla_print()->objects()) {
                ++m_print_object_idx;
                if (po->model_object()->id() == m_model_object->id())
                    break;
            }
        }
        if (m_print_object_idx >= 0) {
            const SLAPrintObject* print_object = m_parent.sla_print()->objects()[m_print_object_idx];

            if (print_object->is_step_done(slaposSupportTree)) {
                // If the supports are already calculated, save the timestamp of the respective step
                // so we can later tell they were recalculated.
                size_t timestamp = print_object->step_state_with_timestamp(slaposSupportTree).timestamp;

                if (!m_supports_tms || (int)timestamp != m_old_timestamp) {
                    // The timestamp has changed - stash the mesh and initialize the TMS.
                    m_supports_mesh = &print_object->support_mesh();
                    m_supports_tms.reset(new TriangleMeshSlicer);
                    // The mesh should already have the shared vertices calculated.
                    m_supports_tms->init(m_supports_mesh, [](){});
                    m_old_timestamp = timestamp;
                }

                // The TMS is initialized - let's do the cutting:
                list_of_expolys.clear();
                m_supports_tms->set_up_direction(up_supports);
                m_supports_tms->slice(std::vector<float>{height_supports}, 0.f, &list_of_expolys, [](){});
                m_supports_triangles = triangulate_expolygons_2f(list_of_expolys[0]);
            }
            else {
                // The supports are not valid. We better dump the cached data.
                m_supports_tms.reset();
                m_supports_triangles.clear();
            }
        }
    }

    // At this point we have the triangulated cuts for both the object and supports - let's render.
	if (! m_triangles.empty()) {
		::glPushMatrix();
		::glTranslated(0.0, 0.0, m_z_shift);
		::glMultMatrixf(instance_matrix.data());
		Eigen::Quaternionf q;
		q.setFromTwoVectors(Vec3f::UnitZ(), up);
		Eigen::AngleAxisf aa(q);
		::glRotatef(aa.angle() * (180./M_PI), aa.axis()(0), aa.axis()(1), aa.axis()(2));
		::glTranslatef(0.f, 0.f, 0.01f); // to make sure the cut does not intersect the structure itself
        ::glColor3f(1.0f, 0.37f, 0.0f);
        ::glBegin(GL_TRIANGLES);
        for (const Vec2f& point : m_triangles)
            ::glVertex3f(point(0), point(1), height_mesh);

        ::glEnd();
		::glPopMatrix();
	}

    if (! m_supports_triangles.empty() && !m_editing_mode) {
        // The supports are hidden in the editing mode, so it makes no sense to render the cuts.
		::glPushMatrix();
        ::glMultMatrixd(supports_trafo.data());
        Eigen::Quaternionf q;
		q.setFromTwoVectors(Vec3f::UnitZ(), up_supports);
		Eigen::AngleAxisf aa(q);
		::glRotatef(aa.angle() * (180./M_PI), aa.axis()(0), aa.axis()(1), aa.axis()(2));
		::glTranslatef(0.f, 0.f, 0.01f);
        ::glColor3f(1.0f, 0.f, 0.37f);
        ::glBegin(GL_TRIANGLES);
        for (const Vec2f& point : m_supports_triangles)
            ::glVertex3f(point(0), point(1), height_supports);

        ::glEnd();
		::glPopMatrix();
	}
}


void GLGizmoSlaSupports::on_render_for_picking(const Selection& selection) const
{
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_points(selection, true);
}

void GLGizmoSlaSupports::render_points(const Selection& selection, bool picking) const
{
    if (!picking)
        glsafe(::glEnable(GL_LIGHTING));

    const GLVolume* vol = selection.get_volume(*selection.get_volume_idxs().begin());
    const Transform3d& instance_scaling_matrix_inverse = vol->get_instance_transformation().get_matrix(true, true, false, true).inverse();
    const Transform3d& instance_matrix = vol->get_instance_transformation().get_matrix();

    glsafe(::glPushMatrix());
    glsafe(::glTranslated(0.0, 0.0, m_z_shift));
    glsafe(::glMultMatrixd(instance_matrix.data()));

    float render_color[3];
    for (int i = 0; i < (int)m_editing_mode_cache.size(); ++i)
    {
        const sla::SupportPoint& support_point = m_editing_mode_cache[i].support_point;
        const bool& point_selected = m_editing_mode_cache[i].selected;

        if (is_point_clipped(support_point.pos.cast<double>()))
            continue;

        // First decide about the color of the point.
        if (picking) {
            std::array<float, 3> color = picking_color_component(i);
            render_color[0] = color[0];
            render_color[1] = color[1];
            render_color[2] = color[2];
        }
        else {
            if ((m_hover_id == i && m_editing_mode)) { // ignore hover state unless editing mode is active
                render_color[0] = 0.f;
                render_color[1] = 1.0f;
                render_color[2] = 1.0f;
            }
            else { // neigher hover nor picking
                bool supports_new_island = m_lock_unique_islands && m_editing_mode_cache[i].support_point.is_new_island;
                if (m_editing_mode) {
                    render_color[0] = point_selected ? 1.0f : (supports_new_island ? 0.3f : 0.7f);
                    render_color[1] = point_selected ? 0.3f : (supports_new_island ? 0.3f : 0.7f);
                    render_color[2] = point_selected ? 0.3f : (supports_new_island ? 1.0f : 0.7f);
                }
                else
                    for (unsigned char i=0; i<3; ++i) render_color[i] = 0.5f;
            }
        }
        glsafe(::glColor3fv(render_color));
        float render_color_emissive[4] = { 0.5f * render_color[0], 0.5f * render_color[1], 0.5f * render_color[2], 1.f};
        glsafe(::glMaterialfv(GL_FRONT, GL_EMISSION, render_color_emissive));

        // Inverse matrix of the instance scaling is applied so that the mark does not scale with the object.
        glsafe(::glPushMatrix());
        glsafe(::glTranslated(support_point.pos(0), support_point.pos(1), support_point.pos(2)));
        glsafe(::glMultMatrixd(instance_scaling_matrix_inverse.data()));

        if (vol->is_left_handed())
            glFrontFace(GL_CW);

        // Matrices set, we can render the point mark now.
        // If in editing mode, we'll also render a cone pointing to the sphere.
        if (m_editing_mode) {
            if (m_editing_mode_cache[i].normal == Vec3f::Zero())
                update_cache_entry_normal(i); // in case the normal is not yet cached, find and cache it

            Eigen::Quaterniond q;
            q.setFromTwoVectors(Vec3d{0., 0., 1.}, instance_scaling_matrix_inverse * m_editing_mode_cache[i].normal.cast<double>());
            Eigen::AngleAxisd aa(q);
            glsafe(::glRotated(aa.angle() * (180. / M_PI), aa.axis()(0), aa.axis()(1), aa.axis()(2)));

            const float cone_radius = 0.25f; // mm
            const float cone_height = 0.75f;
            glsafe(::glPushMatrix());
            glsafe(::glTranslatef(0.f, 0.f, m_editing_mode_cache[i].support_point.head_front_radius * RenderPointScale));
            ::gluCylinder(m_quadric, 0.f, cone_radius, cone_height, 24, 1);
            glsafe(::glTranslatef(0.f, 0.f, cone_height));
            ::gluDisk(m_quadric, 0.0, cone_radius, 24, 1);
            glsafe(::glPopMatrix());
        }
        ::gluSphere(m_quadric, m_editing_mode_cache[i].support_point.head_front_radius * RenderPointScale, 24, 12);
        if (vol->is_left_handed())
            glFrontFace(GL_CCW);

        glsafe(::glPopMatrix());
    }

    {
        // Reset emissive component to zero (the default value)
        float render_color_emissive[4] = { 0.f, 0.f, 0.f, 1.f };
        glsafe(::glMaterialfv(GL_FRONT, GL_EMISSION, render_color_emissive));
    }

    if (!picking)
        glsafe(::glDisable(GL_LIGHTING));

    glsafe(::glPopMatrix());
}



bool GLGizmoSlaSupports::is_point_clipped(const Vec3d& point) const
{
    const Vec3d& direction_to_camera = m_clipping_plane_normal;

    if (m_clipping_plane_distance == 0.f)
        return false;

    Vec3d transformed_point = m_model_object->instances.front()->get_transformation().get_matrix() * point;
    transformed_point(2) += m_z_shift;
    return direction_to_camera.dot(m_model_object->instances[m_active_instance]->get_offset() + Vec3d(0., 0., m_z_shift)) + m_active_instance_bb_radius
            - m_clipping_plane_distance * 2*m_active_instance_bb_radius < direction_to_camera.dot(transformed_point);
}



bool GLGizmoSlaSupports::is_mesh_update_necessary() const
{
    return ((m_state == On) && (m_model_object != nullptr) && !m_model_object->instances.empty())
        && ((m_model_object->id() != m_current_mesh_model_id) || m_its == nullptr);
}

void GLGizmoSlaSupports::update_mesh()
{
    wxBusyCursor wait;
    // this way we can use that mesh directly.
    // This mesh does not account for the possible Z up SLA offset.
    m_mesh = &m_model_object->volumes.front()->mesh();
    m_its = &m_mesh->its;
    m_current_mesh_model_id = m_model_object->id();
    m_editing_mode = false;

	m_AABB.deinit();
    m_AABB.init(
        MapMatrixXfUnaligned(m_its->vertices.front().data(), m_its->vertices.size(), 3),
        MapMatrixXiUnaligned(m_its->indices.front().data(), m_its->indices.size(), 3));
}

// Unprojects the mouse position on the mesh and return the hit point and normal of the facet.
// The function throws if no intersection if found.
std::pair<Vec3f, Vec3f> GLGizmoSlaSupports::unproject_on_mesh(const Vec2d& mouse_pos)
{
    // if the gizmo doesn't have the V, F structures for igl, calculate them first:
    if (m_its == nullptr)
        update_mesh();

    const Camera& camera = m_parent.get_camera();
    const std::array<int, 4>& viewport = camera.get_viewport();
    const Transform3d& modelview_matrix = camera.get_view_matrix();
    const Transform3d& projection_matrix = camera.get_projection_matrix();

    Vec3d point1;
    Vec3d point2;
    ::gluUnProject(mouse_pos(0), viewport[3] - mouse_pos(1), 0.f, modelview_matrix.data(), projection_matrix.data(), viewport.data(), &point1(0), &point1(1), &point1(2));
    ::gluUnProject(mouse_pos(0), viewport[3] - mouse_pos(1), 1.f, modelview_matrix.data(), projection_matrix.data(), viewport.data(), &point2(0), &point2(1), &point2(2));

    std::vector<igl::Hit> hits;

    const Selection& selection = m_parent.get_selection();
    const GLVolume* volume = selection.get_volume(*selection.get_volume_idxs().begin());

    point1(2) -= m_z_shift;
	point2(2) -= m_z_shift;

    Transform3d inv = volume->get_instance_transformation().get_matrix().inverse();

    point1 = inv * point1;
    point2 = inv * point2;

    if (!m_AABB.intersect_ray(
        MapMatrixXfUnaligned(m_its->vertices.front().data(), m_its->vertices.size(), 3),
        MapMatrixXiUnaligned(m_its->indices.front().data(), m_its->indices.size(), 3),
        point1.cast<float>(), (point2-point1).cast<float>(), hits))
        throw std::invalid_argument("unproject_on_mesh(): No intersection found.");

    std::sort(hits.begin(), hits.end(), [](const igl::Hit& a, const igl::Hit& b) { return a.t < b.t; });

    // Now let's iterate through the points and find the first that is not clipped:
    unsigned int i=0;
    Vec3f bc;
    Vec3f a;
    Vec3f b;
    Vec3f result;
    for (i=0; i<hits.size(); ++i) {
        igl::Hit& hit = hits[i];
        int fid = hit.id;   // facet id
        bc = Vec3f(1-hit.u-hit.v, hit.u, hit.v); // barycentric coordinates of the hit
        a = (m_its->vertices[m_its->indices[fid](1)] - m_its->vertices[m_its->indices[fid](0)]);
        b = (m_its->vertices[m_its->indices[fid](2)] - m_its->vertices[m_its->indices[fid](0)]);
        result = bc(0) * m_its->vertices[m_its->indices[fid](0)] + bc(1) * m_its->vertices[m_its->indices[fid](1)] + bc(2)*m_its->vertices[m_its->indices[fid](2)];
        if (m_clipping_plane_distance == 0.f || !is_point_clipped(result.cast<double>()))
            break;
    }

    if (i==hits.size() || (hits.size()-i) % 2 != 0) {
        // All hits are either clipped, or there is an odd number of unclipped
        // hits - meaning the nearest must be from inside the mesh.
        throw std::invalid_argument("unproject_on_mesh(): No intersection found.");
    }

    // Calculate and return both the point and the facet normal.
    return std::make_pair(
            result,
            a.cross(b)
        );
}

// Following function is called from GLCanvas3D to inform the gizmo about a mouse/keyboard event.
// The gizmo has an opportunity to react - if it does, it should return true so that the Canvas3D is
// aware that the event was reacted to and stops trying to make different sense of it. If the gizmo
// concludes that the event was not intended for it, it should return false.
bool GLGizmoSlaSupports::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (m_editing_mode) {

        // left down with shift - show the selection rectangle:
        if (action == SLAGizmoEventType::LeftDown && (shift_down || alt_down || control_down)) {
            if (m_hover_id == -1) {
                if (shift_down || alt_down) {
                    m_selection_rectangle.start_dragging(mouse_position, shift_down ? GLSelectionRectangle::Select : GLSelectionRectangle::Deselect);
                }
            }
            else {
                if (m_editing_mode_cache[m_hover_id].selected)
                    unselect_point(m_hover_id);
                else {
                    if (!alt_down)
                        select_point(m_hover_id);
                }
            }

            return true;
        }

        // left down without selection rectangle - place point on the mesh:
        if (action == SLAGizmoEventType::LeftDown && !m_selection_rectangle.is_dragging() && !shift_down) {
            // If any point is in hover state, this should initiate its move - return control back to GLCanvas:
            if (m_hover_id != -1)
                return false;

            // If there is some selection, don't add new point and deselect everything instead.
            if (m_selection_empty) {
                try {
                    std::pair<Vec3f, Vec3f> pos_and_normal = unproject_on_mesh(mouse_position); // don't create anything if this throws
                    m_editing_mode_cache.emplace_back(sla::SupportPoint(pos_and_normal.first, m_new_point_head_diameter/2.f, false), false, pos_and_normal.second);
                    m_unsaved_changes = true;
                    m_parent.set_as_dirty();
                    m_wait_for_up_event = true;
                }
                catch (...) {   // not clicked on object
                    return false;
                }
            }
            else
                select_point(NoPoints);

            return true;
        }

        // left up with selection rectangle - select points inside the rectangle:
        if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::ShiftUp || action == SLAGizmoEventType::AltUp) && m_selection_rectangle.is_dragging()) {
            // Is this a selection or deselection rectangle?
            GLSelectionRectangle::EState rectangle_status = m_selection_rectangle.get_state();

            // First collect positions of all the points in world coordinates.
            const Transform3d& instance_matrix = m_model_object->instances[m_active_instance]->get_transformation().get_matrix();
            std::vector<Vec3d> points;
            for (unsigned int i=0; i<m_editing_mode_cache.size(); ++i) {
                const sla::SupportPoint &support_point = m_editing_mode_cache[i].support_point;
                points.push_back(instance_matrix * support_point.pos.cast<double>());
                points.back()(2) += m_z_shift;
            }
            // Now ask the rectangle which of the points are inside.
            const Camera& camera = m_parent.get_camera();
            std::vector<unsigned int> selected_idxs = m_selection_rectangle.stop_dragging(m_parent, points);

            // we'll recover current look direction (in world coords) and transform it to model coords.
            const Selection& selection = m_parent.get_selection();
            const GLVolume* volume = selection.get_volume(*selection.get_volume_idxs().begin());
            const Transform3d& instance_matrix_no_translation_no_scaling = volume->get_instance_transformation().get_matrix(true,false,true);
            Vec3f direction_to_camera = -camera.get_dir_forward().cast<float>();
            Vec3f direction_to_camera_mesh = (instance_matrix_no_translation_no_scaling.inverse().cast<float>() * direction_to_camera).normalized().eval();
            Vec3f scaling = volume->get_instance_scaling_factor().cast<float>();
            direction_to_camera_mesh = Vec3f(direction_to_camera_mesh(0)*scaling(0), direction_to_camera_mesh(1)*scaling(1), direction_to_camera_mesh(2)*scaling(2));

            // Iterate over all points in the rectangle and check that they are neither clipped by the
            // clipping plane nor obscured by the mesh.
            for (const unsigned int i : selected_idxs) {
                const sla::SupportPoint &support_point = m_editing_mode_cache[i].support_point;
                if (!is_point_clipped(support_point.pos.cast<double>())) {
                    bool is_obscured = false;
                    // Cast a ray in the direction of the camera and look for intersection with the mesh:
                    std::vector<igl::Hit> hits;
                    // Offset the start of the ray to the front of the ball + EPSILON to account for numerical inaccuracies.
                    if (m_AABB.intersect_ray(
                            MapMatrixXfUnaligned(m_its->vertices.front().data(), m_its->vertices.size(), 3),
                            MapMatrixXiUnaligned(m_its->indices.front().data(), m_its->indices.size(), 3),
                            support_point.pos + direction_to_camera_mesh * (support_point.head_front_radius + EPSILON), direction_to_camera_mesh, hits)) {
                        std::sort(hits.begin(), hits.end(), [](const igl::Hit& h1, const igl::Hit& h2) { return h1.t < h2.t; });

                        if (m_clipping_plane_distance != 0.f) {
                            // If the closest hit facet normal points in the same direction as the ray,
                            // we are looking through the mesh and should therefore discard the point:
                            int fid = hits.front().id;   // facet id
                            Vec3f a = (m_its->vertices[m_its->indices[fid](1)] - m_its->vertices[m_its->indices[fid](0)]);
                            Vec3f b = (m_its->vertices[m_its->indices[fid](2)] - m_its->vertices[m_its->indices[fid](0)]);
                            if ((a.cross(b)).dot(direction_to_camera_mesh) > 0.f)
                                is_obscured = true;

                            // Eradicate all hits that are on clipped surfaces:
                            for (unsigned int j=0; j<hits.size(); ++j) {
                                const igl::Hit& hit = hits[j];
                                int fid = hit.id;   // facet id

                                Vec3f bc = Vec3f(1-hit.u-hit.v, hit.u, hit.v); // barycentric coordinates of the hit
                                Vec3f hit_pos = bc(0) * m_its->vertices[m_its->indices[fid](0)] + bc(1) * m_its->vertices[m_its->indices[fid](1)] + bc(2)*m_its->vertices[m_its->indices[fid](2)];
                                if (is_point_clipped(hit_pos.cast<double>())) {
                                    hits.erase(hits.begin()+j);
                                    --j;
                                }
                            }
                        }

                        // FIXME: the intersection could in theory be behind the camera, but as of now we only have camera direction.
                        // Also, the threshold is in mesh coordinates, not in actual dimensions.
                        if (!hits.empty())
                            is_obscured = true;
                    }

                    if (!is_obscured) {
                        if (rectangle_status == GLSelectionRectangle::Deselect)
                            unselect_point(i);
                        else
                            select_point(i);
                    }
                }
            }
            return true;
        }

        // left up with no selection rectangle
        if (action == SLAGizmoEventType::LeftUp) {
            if (m_wait_for_up_event) {
                m_wait_for_up_event = false;
                return true;
            }
        }

        // dragging the selection rectangle:
        if (action == SLAGizmoEventType::Dragging) {
            if (m_wait_for_up_event)
                return true; // point has been placed and the button not released yet
                             // this prevents GLCanvas from starting scene rotation

            if (m_selection_rectangle.is_dragging())  {
                m_selection_rectangle.dragging(mouse_position);
                return true;
            }

            return false;
        }

        if (action == SLAGizmoEventType::Delete) {
            // delete key pressed
            delete_selected_points();
            return true;
        }

        if (action ==  SLAGizmoEventType::ApplyChanges) {
            editing_mode_apply_changes();
            return true;
        }

        if (action ==  SLAGizmoEventType::DiscardChanges) {
            editing_mode_discard_changes();
            return true;
        }

        if (action == SLAGizmoEventType::RightDown) {
            if (m_hover_id != -1) {
                select_point(NoPoints);
                select_point(m_hover_id);
                delete_selected_points();
                return true;
            }
            return false;
        }

        if (action == SLAGizmoEventType::SelectAll) {
            select_point(AllPoints);
            return true;
        }
    }

    if (!m_editing_mode) {
        if (action == SLAGizmoEventType::AutomaticGeneration) {
            auto_generate();
            return true;
        }

        if (action == SLAGizmoEventType::ManualEditing) {
            switch_to_editing_mode();
            return true;
        }
    }

    if (action == SLAGizmoEventType::MouseWheelUp && control_down) {
        m_clipping_plane_distance = std::min(1.f, m_clipping_plane_distance + 0.01f);
        m_parent.set_as_dirty();
        return true;
    }

    if (action == SLAGizmoEventType::MouseWheelDown && control_down) {
        m_clipping_plane_distance = std::max(0.f, m_clipping_plane_distance - 0.01f);
        m_parent.set_as_dirty();
        return true;
    }

    if (action == SLAGizmoEventType::ResetClippingPlane) {
        reset_clipping_plane_normal();
        return true;
    }

    return false;
}

void GLGizmoSlaSupports::delete_selected_points(bool force)
{
    for (unsigned int idx=0; idx<m_editing_mode_cache.size(); ++idx) {
        if (m_editing_mode_cache[idx].selected && (!m_editing_mode_cache[idx].support_point.is_new_island || !m_lock_unique_islands || force)) {
            m_editing_mode_cache.erase(m_editing_mode_cache.begin() + (idx--));
            m_unsaved_changes = true;
        }
            // This should trigger the support generation
            // wxGetApp().plater()->reslice_SLA_supports(*m_model_object);
    }

    select_point(NoPoints);

    //m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoSlaSupports::on_update(const UpdateData& data, const Selection& selection)
{
    if (m_editing_mode && m_hover_id != -1 && data.mouse_pos && (!m_editing_mode_cache[m_hover_id].support_point.is_new_island || !m_lock_unique_islands)) {
        std::pair<Vec3f, Vec3f> pos_and_normal;
        try {
            pos_and_normal = unproject_on_mesh(Vec2d((*data.mouse_pos)(0), (*data.mouse_pos)(1)));
        }
        catch (...) { return; }
        m_editing_mode_cache[m_hover_id].support_point.pos = pos_and_normal.first;
        m_editing_mode_cache[m_hover_id].support_point.is_new_island = false;
        m_editing_mode_cache[m_hover_id].normal = pos_and_normal.second;
        m_unsaved_changes = true;
        // Do not update immediately, wait until the mouse is released.
        // m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

std::vector<const ConfigOption*> GLGizmoSlaSupports::get_config_options(const std::vector<std::string>& keys) const
{
    std::vector<const ConfigOption*> out;

    if (!m_model_object)
        return out;

    const DynamicPrintConfig& object_cfg = m_model_object->config;
    const DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    std::unique_ptr<DynamicPrintConfig> default_cfg = nullptr;

    for (const std::string& key : keys) {
        if (object_cfg.has(key))
            out.push_back(object_cfg.option(key));
        else
            if (print_cfg.has(key))
                out.push_back(print_cfg.option(key));
            else { // we must get it from defaults
                if (default_cfg == nullptr)
                    default_cfg.reset(DynamicPrintConfig::new_from_defaults_keys(keys));
                out.push_back(default_cfg->option(key));
            }
    }

    return out;
}


void GLGizmoSlaSupports::update_cache_entry_normal(unsigned int i) const
{
    int idx = 0;
    Eigen::Matrix<float, 1, 3> pp = m_editing_mode_cache[i].support_point.pos;
    Eigen::Matrix<float, 1, 3> cc;
    m_AABB.squared_distance(
        MapMatrixXfUnaligned(m_its->vertices.front().data(), m_its->vertices.size(), 3),
        MapMatrixXiUnaligned(m_its->indices.front().data(), m_its->indices.size(), 3),
        pp, idx, cc);
    Vec3f a = (m_its->vertices[m_its->indices[idx](1)] - m_its->vertices[m_its->indices[idx](0)]);
    Vec3f b = (m_its->vertices[m_its->indices[idx](2)] - m_its->vertices[m_its->indices[idx](0)]);
    m_editing_mode_cache[i].normal = a.cross(b);
}




ClippingPlane GLGizmoSlaSupports::get_sla_clipping_plane() const
{
    if (!m_model_object || m_state == Off)
        return ClippingPlane::ClipsNothing();

    //Eigen::Matrix<GLdouble, 4, 4, Eigen::DontAlign> modelview_matrix;
    //::glGetDoublev(GL_MODELVIEW_MATRIX, modelview_matrix.data());
    // we'll recover current look direction from the modelview matrix (in world coords):
    //Vec3d direction_to_camera(modelview_matrix.data()[2], modelview_matrix.data()[6], modelview_matrix.data()[10]);

    const Vec3d& direction_to_camera = m_clipping_plane_normal;
    float dist = direction_to_camera.dot(m_model_object->instances[m_active_instance]->get_offset() + Vec3d(0., 0., m_z_shift));

    return ClippingPlane(-direction_to_camera.normalized(),(dist - (-m_active_instance_bb_radius) - m_clipping_plane_distance * 2*m_active_instance_bb_radius));
}


/*
void GLGizmoSlaSupports::find_intersecting_facets(const igl::AABB<Eigen::MatrixXf, 3>* aabb, const Vec3f& normal, double offset, std::vector<unsigned int>& idxs) const
{
    if (aabb->is_leaf()) { // this is a facet
        // corner.dot(normal) - offset
        idxs.push_back(aabb->m_primitive);
    }
    else { // not a leaf
    using CornerType = Eigen::AlignedBox<float, 3>::CornerType;
        bool sign = std::signbit(offset - normal.dot(aabb->m_box.corner(CornerType(0))));
        for (unsigned int i=1; i<8; ++i)
            if (std::signbit(offset - normal.dot(aabb->m_box.corner(CornerType(i)))) != sign) {
                find_intersecting_facets(aabb->m_left, normal, offset, idxs);
                find_intersecting_facets(aabb->m_right, normal, offset, idxs);
            }
    }
}



void GLGizmoSlaSupports::make_line_segments() const
{
    TriangleMeshSlicer tms(&m_model_object->volumes.front()->mesh);
    Vec3f normal(0.f, 1.f, 1.f);
    double d = 0.;

    std::vector<IntersectionLine> lines;
    find_intersections(&m_AABB, normal, d, lines);
    ExPolygons expolys;
    tms.make_expolygons_simple(lines, &expolys);

    SVG svg("slice_loops.svg", get_extents(expolys));
    svg.draw(expolys);
    //for (const IntersectionLine &l : lines[i])
    //    svg.draw(l, "red", 0);
    //svg.draw_outline(expolygons, "black", "blue", 0);
    svg.Close();
}
*/


void GLGizmoSlaSupports::on_render_input_window(float x, float y, float bottom_limit, const Selection& selection)
{
    if (!m_model_object)
        return;

    bool first_run = true; // This is a hack to redraw the button when all points are removed,
                           // so it is not delayed until the background process finishes.
RENDER_AGAIN:
    //m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    //const ImVec2 window_size(m_imgui->scaled(18.f, 16.f));
    //ImGui::SetNextWindowPos(ImVec2(x, y - std::max(0.f, y+window_size.y-bottom_limit) ));
    //ImGui::SetNextWindowSize(ImVec2(window_size));
    
    const float approx_height = m_imgui->scaled(18.0f);
    y = std::min(y, bottom_limit - approx_height);
    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);
    m_imgui->set_next_window_bg_alpha(0.5f);
    m_imgui->begin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:

    const float settings_sliders_left = std::max(m_imgui->calc_text_size(m_desc.at("minimal_distance")).x, m_imgui->calc_text_size(m_desc.at("points_density")).x) + m_imgui->scaled(1.f);
    const float clipping_slider_left = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x, m_imgui->calc_text_size(m_desc.at("reset_direction")).x) + m_imgui->scaled(1.5f);
    const float diameter_slider_left = m_imgui->calc_text_size(m_desc.at("head_diameter")).x + m_imgui->scaled(1.f);
    const float minimal_slider_width = m_imgui->scaled(4.f);
    const float buttons_width_approx = m_imgui->calc_text_size(m_desc.at("apply_changes")).x + m_imgui->calc_text_size(m_desc.at("discard_changes")).x + m_imgui->scaled(1.5f);
    const float lock_supports_width_approx = m_imgui->calc_text_size(m_desc.at("lock_supports")).x + m_imgui->scaled(2.f);

    float window_width = minimal_slider_width + std::max(std::max(settings_sliders_left, clipping_slider_left), diameter_slider_left);
    window_width = std::max(std::max(window_width, buttons_width_approx), lock_supports_width_approx);


    bool force_refresh = false;
    bool remove_selected = false;
    bool remove_all = false;

    if (m_editing_mode) {

        float diameter_upper_cap = static_cast<ConfigOptionFloat*>(wxGetApp().preset_bundle->sla_prints.get_edited_preset().config.option("support_pillar_diameter"))->value;
        if (m_new_point_head_diameter > diameter_upper_cap)
            m_new_point_head_diameter = diameter_upper_cap;
        m_imgui->text(m_desc.at("head_diameter"));
        ImGui::SameLine(diameter_slider_left);
        ImGui::PushItemWidth(window_width - diameter_slider_left);

        if (ImGui::SliderFloat("", &m_new_point_head_diameter, 0.1f, diameter_upper_cap, "%.1f")) {
            // value was changed
            for (auto& cache_entry : m_editing_mode_cache)
                if (cache_entry.selected) {
                    cache_entry.support_point.head_front_radius = m_new_point_head_diameter / 2.f;
                    m_unsaved_changes = true;
                }
        }

        bool changed = m_lock_unique_islands;
        m_imgui->checkbox(m_desc.at("lock_supports"), m_lock_unique_islands);
        force_refresh |= changed != m_lock_unique_islands;

        m_imgui->disabled_begin(m_selection_empty);
        remove_selected = m_imgui->button(m_desc.at("remove_selected"));
        m_imgui->disabled_end();

        m_imgui->disabled_begin(m_editing_mode_cache.empty());
        remove_all = m_imgui->button(m_desc.at("remove_all"));
        m_imgui->disabled_end();

        m_imgui->text(" "); // vertical gap

        if (m_imgui->button(m_desc.at("apply_changes"))) {
            editing_mode_apply_changes();
            force_refresh = true;
        }
        ImGui::SameLine();
        bool discard_changes = m_imgui->button(m_desc.at("discard_changes"));
        if (discard_changes) {
            editing_mode_discard_changes();
            force_refresh = true;
        }
    }
    else { // not in editing mode:
        m_imgui->text(m_desc.at("minimal_distance"));
        ImGui::SameLine(settings_sliders_left);
        ImGui::PushItemWidth(window_width - settings_sliders_left);

        std::vector<const ConfigOption*> opts = get_config_options({"support_points_density_relative", "support_points_minimal_distance"});
        float density = static_cast<const ConfigOptionInt*>(opts[0])->value;
        float minimal_point_distance = static_cast<const ConfigOptionFloat*>(opts[1])->value;

        bool value_changed = ImGui::SliderFloat("", &minimal_point_distance, 0.f, 20.f, "%.f mm");
        if (value_changed)
            m_model_object->config.opt<ConfigOptionFloat>("support_points_minimal_distance", true)->value = minimal_point_distance;

        m_imgui->text(m_desc.at("points_density"));
        ImGui::SameLine(settings_sliders_left);

        if (ImGui::SliderFloat(" ", &density, 0.f, 200.f, "%.f %%")) {
            value_changed = true;
            m_model_object->config.opt<ConfigOptionInt>("support_points_density_relative", true)->value = (int)density;
        }

        if (value_changed) { // Update side panel
            wxTheApp->CallAfter([]() {
                wxGetApp().obj_settings()->UpdateAndShow(true);
                wxGetApp().obj_list()->update_settings_items();
            });
        }

        bool generate = m_imgui->button(m_desc.at("auto_generate"));

        if (generate)
            auto_generate();

        m_imgui->text("");
        if (m_imgui->button(m_desc.at("manual_editing")))
            switch_to_editing_mode();

        m_imgui->disabled_begin(m_editing_mode_cache.empty());
        remove_all = m_imgui->button(m_desc.at("remove_all"));
        m_imgui->disabled_end();

        // m_imgui->text("");
        // m_imgui->text(m_model_object->sla_points_status == sla::PointsStatus::None ? _(L("No points  (will be autogenerated)")) :
        //              (m_model_object->sla_points_status == sla::PointsStatus::AutoGenerated ? _(L("Autogenerated points (no modifications)")) :
        //              (m_model_object->sla_points_status == sla::PointsStatus::UserModified ? _(L("User-modified points")) :
        //              (m_model_object->sla_points_status == sla::PointsStatus::Generating ? _(L("Generation in progress...")) : "UNKNOWN STATUS"))));
    }


    // Following is rendered in both editing and non-editing mode:
    m_imgui->text("");
    if (m_clipping_plane_distance == 0.f)
        m_imgui->text(m_desc.at("clipping_of_view"));
    else {
        if (m_imgui->button(m_desc.at("reset_direction"))) {
            wxGetApp().CallAfter([this](){
                    reset_clipping_plane_normal();
                });
        }
    }

    ImGui::SameLine(clipping_slider_left);
    ImGui::PushItemWidth(window_width - clipping_slider_left);
    ImGui::SliderFloat("  ", &m_clipping_plane_distance, 0.f, 1.f, "%.2f");


    if (m_imgui->button("?")) {
        wxGetApp().CallAfter([]() {
            SlaGizmoHelpDialog help_dlg;
            help_dlg.ShowModal();
        });
    }

    m_imgui->end();

    if (m_editing_mode != m_old_editing_state) { // user toggled between editing/non-editing mode
        m_parent.toggle_sla_auxiliaries_visibility(!m_editing_mode, m_model_object, m_active_instance);
        force_refresh = true;
    }
    m_old_editing_state = m_editing_mode;

    if (remove_selected || remove_all) {
        force_refresh = false;
        m_parent.set_as_dirty();
        if (remove_all)
            select_point(AllPoints);
        delete_selected_points(remove_all);
        if (remove_all && !m_editing_mode)
            editing_mode_apply_changes();
        if (first_run) {
            first_run = false;
            goto RENDER_AGAIN;
        }
    }

    if (force_refresh)
        m_parent.set_as_dirty();
}

bool GLGizmoSlaSupports::on_is_activable(const Selection& selection) const
{
    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA
        || !selection.is_from_single_instance())
            return false;

    // Check that none of the selected volumes is outside. Only SLA auxiliaries (supports) are allowed outside.
    const Selection::IndicesList& list = selection.get_volume_idxs();
    for (const auto& idx : list)
        if (selection.get_volume(idx)->is_outside && selection.get_volume(idx)->composite_id.volume_id >= 0)
            return false;

    return true;
}

bool GLGizmoSlaSupports::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA);
}

std::string GLGizmoSlaSupports::on_get_name() const
{
    return (_(L("SLA Support Points")) + " [L]").ToUTF8().data();
}

void GLGizmoSlaSupports::on_set_state()
{
        if (m_state == On && m_old_state != On) { // the gizmo was just turned on
            if (is_mesh_update_necessary())
                update_mesh();

            // we'll now reload support points:
            if (m_model_object)
                editing_mode_reload_cache();

            m_parent.toggle_model_objects_visibility(false);
            if (m_model_object)
                m_parent.toggle_model_objects_visibility(true, m_model_object, m_active_instance);

            // Set default head diameter from config.
            const DynamicPrintConfig& cfg = wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
            m_new_point_head_diameter = static_cast<const ConfigOptionFloat*>(cfg.option("support_head_front_diameter"))->value;
        }
        if (m_state == Off && m_old_state != Off) { // the gizmo was just turned Off
            wxGetApp().CallAfter([this]() {
                // Following is called through CallAfter, because otherwise there was a problem
                // on OSX with the wxMessageDialog being shown several times when clicked into.
                if (m_model_object) {
                    if (m_unsaved_changes) {
                        wxMessageDialog dlg(GUI::wxGetApp().mainframe, _(L("Do you want to save your manually edited support points?")) + "\n",
                                            _(L("Save changes?")), wxICON_QUESTION | wxYES | wxNO);
                        if (dlg.ShowModal() == wxID_YES)
                            editing_mode_apply_changes();
                        else
                            editing_mode_discard_changes();
                    }
                }
                m_parent.toggle_model_objects_visibility(true);
                m_editing_mode = false; // so it is not active next time the gizmo opens
                m_editing_mode_cache.clear();
                m_clipping_plane_distance = 0.f;
                // Release triangle mesh slicer and the AABB spatial search structure.
                m_AABB.deinit();
                m_its = nullptr;
                m_tms.reset();
                m_supports_tms.reset();
            });
        }
        m_old_state = m_state;
}



void GLGizmoSlaSupports::on_start_dragging(const Selection& selection)
{
    if (m_hover_id != -1) {
        select_point(NoPoints);
        select_point(m_hover_id);
    }
}



void GLGizmoSlaSupports::select_point(int i)
{
    if (i == AllPoints || i == NoPoints) {
        for (auto& point_and_selection : m_editing_mode_cache)
            point_and_selection.selected = ( i == AllPoints );
        m_selection_empty = (i == NoPoints);

        if (i == AllPoints)
            m_new_point_head_diameter = m_editing_mode_cache[0].support_point.head_front_radius * 2.f;
    }
    else {
        m_editing_mode_cache[i].selected = true;
        m_selection_empty = false;
        m_new_point_head_diameter = m_editing_mode_cache[i].support_point.head_front_radius * 2.f;
    }
}


void GLGizmoSlaSupports::unselect_point(int i)
{
    m_editing_mode_cache[i].selected = false;
    m_selection_empty = true;
    for (const CacheEntry& ce : m_editing_mode_cache) {
        if (ce.selected) {
            m_selection_empty = false;
            break;
        }
    }
}



void GLGizmoSlaSupports::editing_mode_discard_changes()
{
    // If the points were autogenerated, they may not be on the ModelObject yet.
    // Because the user probably messed with the cache, we will get the data
    // from the backend again.

    if (m_model_object->sla_points_status == sla::PointsStatus::AutoGenerated)
        get_data_from_backend();
    else {
        m_editing_mode_cache.clear();
        for (const sla::SupportPoint& point : m_model_object->sla_support_points)
            m_editing_mode_cache.emplace_back(point, false);
    }
        m_editing_mode = false;
        m_unsaved_changes = false;
}



void GLGizmoSlaSupports::editing_mode_apply_changes()
{
    // If there are no changes, don't touch the front-end. The data in the cache could have been
    // taken from the backend and copying them to ModelObject would needlessly invalidate them.
    if (m_unsaved_changes) {
        m_model_object->sla_points_status = sla::PointsStatus::UserModified;
        m_model_object->sla_support_points.clear();
        for (const CacheEntry& cache_entry : m_editing_mode_cache)
            m_model_object->sla_support_points.push_back(cache_entry.support_point);

        // Recalculate support structures once the editing mode is left.
        // m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        // m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().CallAfter([this]() { wxGetApp().plater()->reslice_SLA_supports(*m_model_object); });
    }
    m_editing_mode = false;
    m_unsaved_changes = false;
}



void GLGizmoSlaSupports::editing_mode_reload_cache()
{
    m_editing_mode_cache.clear();
    for (const sla::SupportPoint& point : m_model_object->sla_support_points)
        m_editing_mode_cache.emplace_back(point, false);

    m_unsaved_changes = false;
}



void GLGizmoSlaSupports::get_data_from_backend()
{
    for (const SLAPrintObject* po : m_parent.sla_print()->objects()) {
        if (po->model_object()->id() == m_model_object->id() && po->is_step_done(slaposSupportPoints)) {
            m_editing_mode_cache.clear();
            const std::vector<sla::SupportPoint>& points = po->get_support_points();
            auto mat = po->trafo().inverse().cast<float>();
            for (unsigned int i=0; i<points.size();++i)
                m_editing_mode_cache.emplace_back(sla::SupportPoint(mat * points[i].pos, points[i].head_front_radius, points[i].is_new_island), false);

            if (m_model_object->sla_points_status != sla::PointsStatus::UserModified)
                m_model_object->sla_points_status = sla::PointsStatus::AutoGenerated;

            break;
        }
    }
    m_unsaved_changes = false;

    // We don't copy the data into ModelObject, as this would stop the background processing.
}



void GLGizmoSlaSupports::auto_generate()
{
    wxMessageDialog dlg(GUI::wxGetApp().plater(), _(L(
                "Autogeneration will erase all manually edited points.\n\n"
                "Are you sure you want to do it?\n"
                )), _(L("Warning")), wxICON_WARNING | wxYES | wxNO);

    if (m_model_object->sla_points_status != sla::PointsStatus::UserModified || m_editing_mode_cache.empty() || dlg.ShowModal() == wxID_YES) {
        m_model_object->sla_support_points.clear();
        m_model_object->sla_points_status = sla::PointsStatus::Generating;
        m_editing_mode_cache.clear();
        wxGetApp().CallAfter([this]() { wxGetApp().plater()->reslice_SLA_supports(*m_model_object); });
    }
}



void GLGizmoSlaSupports::switch_to_editing_mode()
{
    if (m_model_object->sla_points_status != sla::PointsStatus::AutoGenerated)
        editing_mode_reload_cache();
    m_unsaved_changes = false;
    m_editing_mode = true;
}



void GLGizmoSlaSupports::reset_clipping_plane_normal() const
{
    Eigen::Matrix<double, 4, 4, Eigen::DontAlign> modelview_matrix;
    ::glGetDoublev(GL_MODELVIEW_MATRIX, modelview_matrix.data());
    m_clipping_plane_normal = Vec3d(modelview_matrix.data()[2], modelview_matrix.data()[6], modelview_matrix.data()[10]);
    m_parent.set_as_dirty();
}


SlaGizmoHelpDialog::SlaGizmoHelpDialog()
: wxDialog(NULL, wxID_ANY, _(L("SLA gizmo keyboard shortcuts")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER)
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    const wxString ctrl = GUI::shortkey_ctrl_prefix();
    const wxString alt  = GUI::shortkey_alt_prefix();


    // fonts
    const wxFont& font = wxGetApp().small_font();
    const wxFont& bold_font = wxGetApp().bold_font();

    auto note_text = new wxStaticText(this, wxID_ANY, _(L("Note: some shortcuts work in (non)editing mode only.")));
    note_text->SetFont(font);

    auto vsizer    = new wxBoxSizer(wxVERTICAL);
    auto gridsizer = new wxFlexGridSizer(2, 5, 15);
    auto hsizer    = new wxBoxSizer(wxHORIZONTAL);

    hsizer->AddSpacer(20);
    hsizer->Add(vsizer);
    hsizer->AddSpacer(20);

    vsizer->AddSpacer(20);
    vsizer->Add(note_text, 1, wxALIGN_CENTRE_HORIZONTAL);
    vsizer->AddSpacer(20);
    vsizer->Add(gridsizer);
    vsizer->AddSpacer(20);

    std::vector<std::pair<wxString, wxString>> shortcuts;
    shortcuts.push_back(std::make_pair(_(L("Left click")),          _(L("Add point"))));
    shortcuts.push_back(std::make_pair(_(L("Right click")),         _(L("Remove point"))));
    shortcuts.push_back(std::make_pair(_(L("Drag")),                _(L("Move point"))));
    shortcuts.push_back(std::make_pair(ctrl+_(L("Left click")),     _(L("Add point to selection"))));
    shortcuts.push_back(std::make_pair(alt+_(L("Left click")),      _(L("Remove point from selection"))));
    shortcuts.push_back(std::make_pair(wxString("Shift+")+_(L("Drag")), _(L("Select by rectangle"))));
    shortcuts.push_back(std::make_pair(alt+_(L("Drag")),            _(L("Deselect by rectangle"))));
    shortcuts.push_back(std::make_pair(ctrl+"A",                    _(L("Select all points"))));
    shortcuts.push_back(std::make_pair("Delete",                    _(L("Remove selected points"))));
    shortcuts.push_back(std::make_pair(ctrl+_(L("Mouse wheel")),    _(L("Move clipping plane"))));
    shortcuts.push_back(std::make_pair("R",                         _(L("Reset clipping plane"))));
    shortcuts.push_back(std::make_pair("Enter",                     _(L("Apply changes"))));
    shortcuts.push_back(std::make_pair("Esc",                       _(L("Discard changes"))));
    shortcuts.push_back(std::make_pair("M",                         _(L("Switch to editing mode"))));
    shortcuts.push_back(std::make_pair("A",                         _(L("Auto-generate points"))));

    for (const auto& pair : shortcuts) {
        auto shortcut = new wxStaticText(this, wxID_ANY, pair.first);
        auto desc = new wxStaticText(this, wxID_ANY, pair.second);
        shortcut->SetFont(bold_font);
        desc->SetFont(font);
        gridsizer->Add(shortcut, -1, wxALIGN_CENTRE_VERTICAL);
        gridsizer->Add(desc, -1, wxALIGN_CENTRE_VERTICAL);
    }

    SetSizer(hsizer);
    hsizer->SetSizeHints(this);
}



} // namespace GUI
} // namespace Slic3r