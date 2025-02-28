#ifndef slic3r_GLGizmoSlaSupports_hpp_
#define slic3r_GLGizmoSlaSupports_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmos.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"

// There is an L function in igl that would be overridden by our localization macro - let's undefine it...
#undef L
#include <igl/AABB.h>
#include "slic3r/GUI/I18N.hpp"  // ...and redefine again when we are done with the igl code

#include "libslic3r/SLA/SLACommon.hpp"
#include "libslic3r/SLAPrint.hpp"
#include <wx/dialog.h>


namespace Slic3r {
namespace GUI {


class ClippingPlane;


class GLGizmoSlaSupports : public GLGizmoBase
{
private:
    ModelObject* m_model_object = nullptr;
    ModelID m_current_mesh_model_id = 0;
    int m_active_instance = -1;
    float m_active_instance_bb_radius; // to cache the bb
    mutable float m_z_shift = 0.f;
    std::pair<Vec3f, Vec3f> unproject_on_mesh(const Vec2d& mouse_pos);

    const float RenderPointScale = 1.f;

    GLUquadricObj* m_quadric;
    typedef Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>> MapMatrixXfUnaligned;
    typedef Eigen::Map<const Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>> MapMatrixXiUnaligned;
    igl::AABB<MapMatrixXfUnaligned, 3> m_AABB;
    const TriangleMesh* m_mesh;
    const indexed_triangle_set* m_its;
    mutable const TriangleMesh* m_supports_mesh;
    mutable std::vector<Vec2f> m_triangles;
    mutable std::vector<Vec2f> m_supports_triangles;
    mutable int m_old_timestamp = -1;
    mutable int m_print_object_idx = -1;
    mutable int m_print_objects_count = -1;

    class CacheEntry {
    public:
        CacheEntry(const sla::SupportPoint& point, bool sel, const Vec3f& norm = Vec3f::Zero()) :
            support_point(point), selected(sel), normal(norm) {}

        sla::SupportPoint support_point;
        bool selected; // whether the point is selected
        Vec3f normal;
    };

public:
#if ENABLE_SVG_ICONS
    GLGizmoSlaSupports(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
#else
    GLGizmoSlaSupports(GLCanvas3D& parent, unsigned int sprite_id);
#endif // ENABLE_SVG_ICONS
    virtual ~GLGizmoSlaSupports();
    void set_sla_support_data(ModelObject* model_object, const Selection& selection);
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);
    void delete_selected_points(bool force = false);
    ClippingPlane get_sla_clipping_plane() const;

    bool is_in_editing_mode() const { return m_editing_mode; }
    bool is_selection_rectangle_dragging() const { return m_selection_rectangle.is_dragging(); }

private:
    bool on_init();
    void on_update(const UpdateData& data, const Selection& selection);
    virtual void on_render(const Selection& selection) const;
    virtual void on_render_for_picking(const Selection& selection) const;

    //void render_selection_rectangle() const;
    void render_points(const Selection& selection, bool picking = false) const;
    void render_clipping_plane(const Selection& selection) const;
    bool is_mesh_update_necessary() const;
    void update_mesh();
    void update_cache_entry_normal(unsigned int i) const;

    bool m_lock_unique_islands = false;
    bool m_editing_mode = false;            // Is editing mode active?
    bool m_old_editing_state = false;       // To keep track of whether the user toggled between the modes (needed for imgui refreshes).
    float m_new_point_head_diameter;        // Size of a new point.
    float m_minimal_point_distance = 20.f;
    mutable std::vector<CacheEntry> m_editing_mode_cache; // a support point and whether it is currently selected
    float m_clipping_plane_distance = 0.f;
    mutable float m_old_clipping_plane_distance = 0.f;
    mutable Vec3d m_old_clipping_plane_normal;
    mutable Vec3d m_clipping_plane_normal = Vec3d::Zero();

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;

    GLSelectionRectangle m_selection_rectangle;

    bool m_wait_for_up_event = false;
    bool m_unsaved_changes = false; // Are there unsaved changes in manual mode?
    bool m_selection_empty = true;
    EState m_old_state = Off; // to be able to see that the gizmo has just been closed (see on_set_state)

    mutable std::unique_ptr<TriangleMeshSlicer> m_tms;
    mutable std::unique_ptr<TriangleMeshSlicer> m_supports_tms;

    std::vector<const ConfigOption*> get_config_options(const std::vector<std::string>& keys) const;
    bool is_point_clipped(const Vec3d& point) const;
    //void find_intersecting_facets(const igl::AABB<Eigen::MatrixXf, 3>* aabb, const Vec3f& normal, double offset, std::vector<unsigned int>& out) const;

    // Methods that do the model_object and editing cache synchronization,
    // editing mode selection, etc:
    enum {
        AllPoints = -2,
        NoPoints,
    };
    void select_point(int i);
    void unselect_point(int i);
    void editing_mode_apply_changes();
    void editing_mode_discard_changes();
    void editing_mode_reload_cache();
    void get_data_from_backend();
    void auto_generate();
    void switch_to_editing_mode();
    void reset_clipping_plane_normal() const;

protected:
    void on_set_state() override;
    virtual void on_set_hover_id()
    {
        if ((int)m_editing_mode_cache.size() <= m_hover_id)
            m_hover_id = -1;
    }
    void on_start_dragging(const Selection& selection) override;
    virtual void on_render_input_window(float x, float y, float bottom_limit, const Selection& selection) override;

    virtual std::string on_get_name() const;
    virtual bool on_is_activable(const Selection& selection) const;
    virtual bool on_is_selectable() const;
};


class SlaGizmoHelpDialog : public wxDialog
{
public:
    SlaGizmoHelpDialog();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSlaSupports_hpp_
