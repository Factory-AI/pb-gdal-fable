#pragma once

// Minimal declarations for the stable PROJ C API (system libproj.so.22,
// PROJ 8.2.1), written from the public API documentation.

extern "C"
{
    typedef struct PJconsts PJ;
    typedef struct pj_ctx PJ_CONTEXT;

    typedef enum
    {
        PJ_TYPE_UNKNOWN = 0,
        PJ_TYPE_ELLIPSOID,
        PJ_TYPE_PRIME_MERIDIAN,
        PJ_TYPE_GEODETIC_REFERENCE_FRAME,
        PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME,
        PJ_TYPE_VERTICAL_REFERENCE_FRAME,
        PJ_TYPE_DYNAMIC_VERTICAL_REFERENCE_FRAME,
        PJ_TYPE_DATUM_ENSEMBLE,
        PJ_TYPE_CRS,
        PJ_TYPE_GEODETIC_CRS,
        PJ_TYPE_GEOCENTRIC_CRS,
        PJ_TYPE_GEOGRAPHIC_CRS,
        PJ_TYPE_GEOGRAPHIC_2D_CRS,
        PJ_TYPE_GEOGRAPHIC_3D_CRS,
        PJ_TYPE_VERTICAL_CRS,
        PJ_TYPE_PROJECTED_CRS,
        PJ_TYPE_COMPOUND_CRS,
        PJ_TYPE_TEMPORAL_CRS,
        PJ_TYPE_ENGINEERING_CRS,
        PJ_TYPE_BOUND_CRS,
        PJ_TYPE_OTHER_CRS,
        PJ_TYPE_CONVERSION,
        PJ_TYPE_TRANSFORMATION,
        PJ_TYPE_CONCATENATED_OPERATION,
        PJ_TYPE_OTHER_COORDINATE_OPERATION,
        PJ_TYPE_TEMPORAL_DATUM,
        PJ_TYPE_ENGINEERING_DATUM,
        PJ_TYPE_PARAMETRIC_DATUM
    } PJ_TYPE;

    typedef enum
    {
        PJ_WKT2_2015 = 0,
        PJ_WKT2_2015_SIMPLIFIED,
        PJ_WKT2_2019,
        PJ_WKT2_2019_SIMPLIFIED,
        PJ_WKT1_GDAL,
        PJ_WKT1_ESRI
    } PJ_WKT_TYPE;

    typedef enum
    {
        PJ_CATEGORY_ELLIPSOID = 0,
        PJ_CATEGORY_PRIME_MERIDIAN,
        PJ_CATEGORY_DATUM,
        PJ_CATEGORY_CRS,
        PJ_CATEGORY_COORDINATE_OPERATION,
        PJ_CATEGORY_DATUM_ENSEMBLE
    } PJ_CATEGORY;

    typedef enum
    {
        PJ_FWD = 1,
        PJ_IDENT = 0,
        PJ_INV = -1
    } PJ_DIRECTION;

    typedef struct
    {
        double x, y, z, t;
    } PJ_XYZT;
    typedef union
    {
        PJ_XYZT xyzt;
        double v[4];
    } PJ_COORD;

    PJ_CONTEXT *proj_context_create(void);
    void proj_context_destroy(PJ_CONTEXT *);
    void proj_context_use_proj4_init_rules(PJ_CONTEXT *, int);
    int proj_log_level(PJ_CONTEXT *, int);
    void proj_context_set_search_paths(PJ_CONTEXT *, int count,
                                       const char *const *paths);
    const char *proj_context_get_database_path(PJ_CONTEXT *);

    typedef char **PROJ_STRING_LIST;
    void proj_string_list_destroy(PROJ_STRING_LIST list);
    PROJ_STRING_LIST proj_get_codes_from_database(PJ_CONTEXT *,
                                                  const char *auth_name,
                                                  PJ_TYPE type,
                                                  int allow_deprecated);

    PJ *proj_create(PJ_CONTEXT *, const char *definition);
    PJ *proj_create_from_wkt(PJ_CONTEXT *, const char *wkt,
                             const char *const *options,
                             PROJ_STRING_LIST *out_warnings,
                             PROJ_STRING_LIST *out_grammar_errors);
    PJ *proj_create_from_database(PJ_CONTEXT *, const char *auth,
                                  const char *code, PJ_CATEGORY category,
                                  int usePROJAlternativeGridNames,
                                  const char *const *options);
    void proj_destroy(PJ *);
    PJ *proj_clone(PJ_CONTEXT *, const PJ *);

    PJ_TYPE proj_get_type(const PJ *);
    const char *proj_get_name(const PJ *);
    const char *proj_get_id_auth_name(const PJ *, int index);
    const char *proj_get_id_code(const PJ *, int index);
    int proj_get_area_of_use(PJ_CONTEXT *, const PJ *, double *west,
                             double *south, double *east, double *north,
                             const char **name);
    const char *proj_as_wkt(PJ_CONTEXT *, const PJ *, PJ_WKT_TYPE type,
                            const char *const *options);
    const char *proj_as_projjson(PJ_CONTEXT *, const PJ *,
                                 const char *const *options);
    const char *proj_as_proj_string(PJ_CONTEXT *, const PJ *, int type,
                                    const char *const *options);

    PJ *proj_crs_get_coordinate_system(PJ_CONTEXT *, const PJ *);
    int proj_cs_get_axis_count(PJ_CONTEXT *, const PJ *);
    int proj_cs_get_axis_info(PJ_CONTEXT *, const PJ *, int index,
                              const char **name, const char **abbrev,
                              const char **direction, double *unit_conv_factor,
                              const char **unit_name, const char **unit_auth,
                              const char **unit_code);
    PJ *proj_crs_get_datum(PJ_CONTEXT *, const PJ *);
    PJ *proj_crs_get_datum_ensemble(PJ_CONTEXT *, const PJ *);
    PJ *proj_crs_get_datum_forced(PJ_CONTEXT *, const PJ *);
    PJ *proj_crs_get_horizontal_datum(PJ_CONTEXT *, const PJ *);
    PJ *proj_get_ellipsoid(PJ_CONTEXT *, const PJ *);
    int proj_uom_get_info_from_database(PJ_CONTEXT *, const char *auth_name,
                                        const char *code,
                                        const char **out_name,
                                        double *out_conv_factor,
                                        const char **out_category);
    int proj_ellipsoid_get_parameters(PJ_CONTEXT *, const PJ *,
                                      double *semi_major, double *semi_minor,
                                      int *is_semi_minor_computed,
                                      double *inv_flattening);
    PJ *proj_get_prime_meridian(PJ_CONTEXT *, const PJ *);
    int proj_prime_meridian_get_parameters(PJ_CONTEXT *, const PJ *,
                                           double *longitude,
                                           double *unit_conv_factor,
                                           const char **unit_name);
    PJ *proj_crs_get_sub_crs(PJ_CONTEXT *, const PJ *, int index);
    PJ *proj_create_compound_crs(PJ_CONTEXT *, const char *crs_name,
                                 PJ *horiz_crs, PJ *vert_crs);
    typedef enum
    {
        PJ_ELLPS2D_LONGITUDE_LATITUDE = 0,
        PJ_ELLPS2D_LATITUDE_LONGITUDE
    } PJ_ELLIPSOIDAL_CS_2D_TYPE;
    PJ *proj_create_ellipsoidal_2D_cs(PJ_CONTEXT *,
                                      PJ_ELLIPSOIDAL_CS_2D_TYPE type,
                                      const char *unit_name,
                                      double unit_conv_factor);
    PJ *proj_create_geographic_crs_from_datum(PJ_CONTEXT *,
                                              const char *crs_name,
                                              PJ *datum_or_datum_ensemble,
                                              PJ *ellipsoidal_cs);
    PJ *proj_create_conversion_equidistant_cylindrical(
        PJ_CONTEXT *, double center_lat, double center_long,
        double false_easting, double false_northing,
        const char *ang_unit_name, double ang_unit_conv_factor,
        const char *linear_unit_name, double linear_unit_conv_factor);
    PJ *proj_create_projected_crs(PJ_CONTEXT *, const char *crs_name,
                                  const PJ *geodetic_crs,
                                  const PJ *conversion,
                                  const PJ *coordinate_system);
    PJ *proj_crs_get_coordoperation(PJ_CONTEXT *, const PJ *);
    int proj_coordoperation_get_method_info(PJ_CONTEXT *, const PJ *,
                                            const char **method_name,
                                            const char **method_auth_name,
                                            const char **method_code);
    int proj_coordoperation_get_param_count(PJ_CONTEXT *, const PJ *);
    int proj_coordoperation_get_param(
        PJ_CONTEXT *, const PJ *, int index, const char **out_name,
        const char **out_auth_name, const char **out_code,
        double *out_value, const char **out_value_string,
        double *out_unit_conv_factor, const char **out_unit_name,
        const char **out_unit_auth_name, const char **out_unit_code,
        const char **out_unit_category);
    PJ *proj_get_source_crs(PJ_CONTEXT *, const PJ *);
    PJ *proj_get_target_crs(PJ_CONTEXT *, const PJ *);

    PJ *proj_alter_id(PJ_CONTEXT *, const PJ *, const char *auth_name,
                      const char *code);
    PJ *proj_crs_alter_geodetic_crs(PJ_CONTEXT *, const PJ *,
                                    const PJ *new_geod_crs);
    PJ *proj_crs_alter_cs_angular_unit(PJ_CONTEXT *, const PJ *,
                                       const char *angular_units,
                                       double angular_units_conv,
                                       const char *unit_auth_name,
                                       const char *unit_code);
    PJ *proj_crs_alter_cs_linear_unit(PJ_CONTEXT *, const PJ *,
                                      const char *linear_units,
                                      double linear_units_conv,
                                      const char *unit_auth_name,
                                      const char *unit_code);
    PJ *proj_crs_alter_parameters_linear_unit(
        PJ_CONTEXT *, const PJ *, const char *linear_units,
        double linear_units_conv, const char *unit_auth_name,
        const char *unit_code, int convert_to_new_unit);
    PJ *proj_crs_promote_to_3D(PJ_CONTEXT *, const char *crs_3D_name,
                               const PJ *crs);
    PJ *proj_crs_create_projected_3D_crs_from_2D(
        PJ_CONTEXT *, const char *crs_name, const PJ *projected_2D_crs,
        const PJ *geog_3D_crs);
    PJ *proj_alter_id(PJ_CONTEXT *, const PJ *, const char *auth_name,
                      const char *code);

    PJ *proj_create_crs_to_crs_from_pj(PJ_CONTEXT *, const PJ *source,
                                       const PJ *target, void *area,
                                       const char *const *options);
    PJ *proj_normalize_for_visualization(PJ_CONTEXT *, const PJ *);
    PJ_COORD proj_trans(PJ *, PJ_DIRECTION, PJ_COORD);
    PJ_COORD proj_coord(double x, double y, double z, double t);
    int proj_errno(const PJ *);
    int proj_errno_reset(const PJ *);
    typedef void (*PJ_LOG_FUNCTION)(void *, int, const char *);
    void proj_log_func(PJ_CONTEXT *, void *app_data, PJ_LOG_FUNCTION);

    int proj_is_crs(const PJ *);
    PJ *proj_crs_get_geodetic_crs(PJ_CONTEXT *, const PJ *);
    int proj_is_equivalent_to_with_ctx(PJ_CONTEXT *, const PJ *, const PJ *,
                                       int criterion);

    typedef struct PJ_OBJ_LIST PJ_OBJ_LIST;
    int proj_is_deprecated(const PJ *);
    PJ_OBJ_LIST *proj_get_non_deprecated(PJ_CONTEXT *, const PJ *);
    PJ_OBJ_LIST *proj_identify(PJ_CONTEXT *, const PJ *,
                               const char *auth_name,
                               const char *const *options,
                               int **out_confidence);
    void proj_int_list_destroy(int *);
    int proj_list_get_count(const PJ_OBJ_LIST *);
    PJ *proj_list_get(PJ_CONTEXT *, const PJ_OBJ_LIST *, int index);
    void proj_list_destroy(PJ_OBJ_LIST *);
    PJ *proj_crs_demote_to_2D(PJ_CONTEXT *, const char *crs_2D_name,
                              const PJ *crs);

    typedef struct
    {
        char *auth_name;
        char *code;
        char *name;
        PJ_TYPE type;
        int deprecated;
        int bbox_valid;
        double west_lon_degree;
        double south_lat_degree;
        double east_lon_degree;
        double north_lat_degree;
        char *area_name;
        char *projection_method_name;
        char *celestial_body_name;
    } PROJ_CRS_INFO;

    PROJ_CRS_INFO **proj_get_crs_info_list_from_database(
        PJ_CONTEXT *, const char *auth_name, const void *params,
        int *out_result_count);
    void proj_crs_info_list_destroy(PROJ_CRS_INFO **list);
}
