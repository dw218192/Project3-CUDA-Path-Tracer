#include <iostream>
#include "scene.h"
#include <cstring>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtc/epsilon.hpp>
#include "TinyObjLoader/tiny_obj_loader.h"
#include "stb_image.h"
#include "ColorConsole/color.hpp"

// if load_render_state flag is true, then the camera & renderstate will be initialized from the file
// otherwise they must be initialized manually
Scene::Scene(string filename, bool load_render_state) : filename(filename) {
    cout << "Reading scene from " << filename << " ..." << endl;
    cout << " " << endl;

    fp_in.open(filename);
    if (!fp_in.is_open()) {
        cout << "Error reading from file - aborting!" << endl;
        throw;
    }

    while (fp_in.good()) {
        string line;
        utilityCore::safeGetline(fp_in, line);
        if (!line.empty()) {
            vector<string> tokens = utilityCore::tokenizeString(line);
            if (tokens[0] == "OBJECT") {
                loadGeom();
                cout << " " << endl;
            } else if (tokens[0] == "CAMERA" && load_render_state) {
                loadCamera();
                cout << " " << endl;
            }
        }
    }
}

Scene::~Scene() {

}

static bool initMaterial(
    Scene& self,
    Material& ret,
    string const& obj_dir,
    tinyobj::material_t const& tinyobj_mat
) {
    Material mat;
    mat.diffuse = color_t(tinyobj_mat.diffuse[0], tinyobj_mat.diffuse[1], tinyobj_mat.diffuse[2]);
    mat.specular.color = color_t(tinyobj_mat.specular[0], tinyobj_mat.specular[1], tinyobj_mat.specular[2]);
    mat.specular.exponent = tinyobj_mat.shininess;
    
    auto& params = tinyobj_mat.unknown_parameter;

#define PARSE_F(name_str, field, default_val)\
    do {\
    if (params.count(name_str)) field = stod(params.find(name_str)->second);\
    else field = default_val;\
    } while(0)

    PARSE_F("refl", mat.hasReflective, 0);
    PARSE_F("refr", mat.hasRefractive, 0);
    PARSE_F("ior", mat.ior, 1);
    PARSE_F("emit", mat.emittance, 0);
    PARSE_F("rough", mat.roughness, 0);

#undef PARSE_F

    auto load_texture = [&](int& id, string const& texname) {
        if (!texname.empty()) {
            if (self.tex_name_to_id.count(texname)) {
                mat.textures.diffuse = self.tex_name_to_id[texname];
            } else {
                int x, y, n;
                string texpath = obj_dir + '/' + texname;

                unsigned char* data = stbi_load(texpath.c_str(), &x, &y, &n, NUM_TEX_CHANNEL);
                if (!data) {
                    return false;
                }

                self.textures.emplace_back(x, y, data);
                stbi_image_free(data);

                id = self.tex_name_to_id[texname] = self.textures.size() - 1;
            }
        } else {
            id = -1;
        }

        return true;
    };

    if (!load_texture(mat.textures.diffuse, tinyobj_mat.diffuse_texname)) {
        return false;
    }
    if (!load_texture(mat.textures.bump, tinyobj_mat.bump_texname)) {
        return false;
    }

    // TODO deduce material type
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif

    if(mat.hasReflective > 0 && mat.hasRefractive > 0) {
        mat.type = Material::Type::GLOSSY;
    } else if (mat.hasReflective > 0) {
        mat.type = Material::Type::REFL;
    } else if (mat.hasRefractive > 0) {
        if (abs(mat.hasRefractive - 1) <= EPSILON) {
            mat.type = Material::Type::TRANSPARENT;
        } else {
            mat.type = Material::Type::REFR;
        }
    } else {
        mat.type = Material::Type::DIFFUSE;
    }

    if (mat.type == Material::Type::DIFFUSE || mat.type == Material::Type::GLOSSY) {
        if (mat.roughness <= EPSILON && mat.roughness >= -EPSILON) {
            mat.roughness = 1; // force roughness for diffuse and glossy material
        }
    }

    cout << "loaded material " << tinyobj_mat.name << endl
        << "diffuse =     {" << mat.diffuse[0] << "," << mat.diffuse[1] << "," << mat.diffuse[2] << "}\n"
        << "emittance =    " << mat.emittance << "\n"
        << "ior =          " << mat.ior << "\n"
        << "refl =         " << mat.hasReflective << "\n"
        << "refr =         " << mat.hasRefractive << "\n"
        << "roughness =    " << mat.roughness << "\n"
        << "spec_color=   {" << mat.specular.color[0] << "," << mat.specular.color[1] << "," << mat.specular.color[2] << "}\n"
        << "spec_exp   =   " << mat.specular.exponent << "\n\n";

    if(mat.textures.diffuse != -1)
        cout << "diffuse tex = {" << " id = " << mat.textures.diffuse << ", npixels = " << self.textures[mat.textures.diffuse].pixels.size() << "}\n";
    if(mat.textures.bump != -1)
        cout << "bump tex    = {" << " id = " << mat.textures.bump << ", npixels = " << self.textures[mat.textures.bump].pixels.size() << "}\n";


    ret = move(mat);
    return true;
}

int Scene::loadGeom() {
    int objectid = geoms.size();
    cout << "Loading Geom " << objectid << "..." << endl;
    Geom newGeom;
    string line;

    //load object type
    utilityCore::safeGetline(fp_in, line);
    if (!line.empty() && fp_in.good()) {
        if (strcmp(line.c_str(), "sphere") == 0) {
            cout << "Creating new sphere..." << endl;
            newGeom.type = SPHERE;
        } else if (strcmp(line.c_str(), "cube") == 0) {
            cout << "Creating new cube..." << endl;
            newGeom.type = CUBE;
        } else {
            newGeom.type = MESH;

            // mesh objects are in the fomat: [file type] [path to file]
            vector<string> tokens = utilityCore::tokenizeString(line);
            if (tokens.size() != 2) {
                cerr << dye::red("ERROR: unrecognized object type\nat line: ") << line << endl;
                return -1;
            }
            if (tokens[0] == "obj") {
                cout << "Loading obj mesh " << tokens[1] << endl;
                size_t pos = tokens[1].find_last_of('/');
                if (pos == string::npos) {
                    cerr << dye::red("ERROR: invalid obj file path: " + tokens[1]) << endl;
                    return -1;
                }
                
                // default material folder to the folder where the mesh is in
                tinyobj::ObjReaderConfig config;
                config.mtl_search_path = tokens[1].substr(0, pos);
                cout << "set material lookup path to: " << config.mtl_search_path << endl;
                

                tinyobj::ObjReader reader;
                if (!reader.ParseFromFile(tokens[1], config)) {
                    if (!reader.Error().empty()) {
                        cerr << dye::red("TinyObjReader: ERROR: \n");
                        cerr << dye::red(reader.Error()) << endl;
                    } else {
                        cerr << dye::red("no idea what the hell is happening\n");
                    }
                    return -1;
                }
                if (!reader.Warning().empty()) {
                    cerr << dye::yellow("TinyObjReader: WARNING: \n");
                    cerr << dye::yellow(reader.Warning()) << endl;
                }

                size_t vert_offset = vertices.size();
                size_t norm_offset = normals.size();
                size_t uv_offset = uvs.size();
                size_t mat_offset = materials.size();
                size_t tan_offset = tangents.size();

                auto const& attrib = reader.GetAttrib();
                auto const& mats = reader.GetMaterials();

                bool has_normal_map = false;
                
                // fill materials
                for (auto const& mat : mats) {
                    Material material;
                    if (!initMaterial(*this, material, config.mtl_search_path, mat)) {
                        cerr << dye::red("FATAL ERROR: mesh " + tokens[1] + " is missing some texture files:\n" +
                           mat.diffuse_texname + "\nresulting in an incomplete state of the loader, exiting...\n");
                        exit(EXIT_FAILURE);
                        return -1;
                    }
                    if (material.textures.bump != -1) {
                        has_normal_map = true;
                    }
                    materials.emplace_back(material);
                }

                // fill vertices
                for (size_t i = 2; i < attrib.vertices.size(); i += 3) {
                    vertices.emplace_back(
                        attrib.vertices[i - 2],
                        attrib.vertices[i - 1],
                        attrib.vertices[i]
                    );
                }
                // fill normals
                for (size_t i = 2; i < attrib.normals.size(); i += 3) {
                    normals.emplace_back(
                        attrib.normals[i - 2],
                        attrib.normals[i - 1],
                        attrib.normals[i]
                    );
                }
                // fill uvs
                for (size_t i = 1; i < attrib.texcoords.size(); i += 2) {
                    uvs.emplace_back(
                        attrib.texcoords[i - 1],
                        attrib.texcoords[i]
                    );
                }

                // temp buffer to compute tangent and bitangent
                int num_verts = attrib.vertices.size() / 3;
                vector<glm::vec3> tan1(num_verts, glm::vec3(0));
                vector<glm::vec3> tan2(num_verts, glm::vec3(0));
                vector<glm::vec3> vert2norm(num_verts);

                // maps normal to normal buffer index (used only if the model is missing normals)
                auto h = [](glm::vec3 const& v)->size_t {
                    auto hs = hash<float>();
                    return hs(v.x) ^ hs(v.y) ^ hs(v.z);
                };
                auto eq = [](glm::vec3 const& v1, glm::vec3 const& v2)->bool {
                    for (int i = 0; i < 3; ++i) {
                        if (!glm::epsilonEqual(v1[i], v2[i], 0.001f)) {
                            return false;
                        }
                    }
                    return true;
                };
                unordered_map<glm::vec3,int,decltype(h),decltype(eq)> normal_deduction_mp(10,h,eq);
                auto add_or_get_norm_id = [&](glm::vec3 const& v) {
                    if (normal_deduction_mp.count(v)) {
                        return normal_deduction_mp[v];
                    } else {
                        int ret = normal_deduction_mp[v] = normals.size();
                        normals.emplace_back(v);
                        return ret;
                    }
                };

                auto compute_tan1tan2 = [&](glm::ivec3 iverts, glm::ivec3 iuvs) {
                    // reference: Lengyel, Eric. "Computing Tangent Space Basis Vectors for an Arbitrary Mesh."
                    // Terathon Software 3D Graphics Library, 2001. http://www.terathon.com/code/tangent.html
                    int i1 = iverts[0], i2 = iverts[1], i3 = iverts[2];
                    glm::vec3 v1 = vertices[iverts[1]] - vertices[iverts[0]];
                    glm::vec3 v2 = vertices[iverts[2]] - vertices[iverts[0]];
                    glm::vec2 u1 = uvs[iuvs[1]] - uvs[iuvs[0]];
                    glm::vec2 u2 = uvs[iuvs[2]] - uvs[iuvs[0]];
                    float f = 1.0f / (u1.x * u2.y - u2.x * u1.y);
                    glm::vec3 sd = (v1 * u2.y - v2 * u1.y) * f;
                    glm::vec3 td = (v2 * u1.x - v1 * u2.x) * f;

                    for (int i = 0; i < 3; ++i) {
                        tan1[iverts[i] - vert_offset] += sd;
                        tan2[iverts[i] - vert_offset] += td;
                    }
                };

                int triangles_start = triangles.size();
                bool missing_norm = false;
                bool missing_uv = false;
                for (auto const& s : reader.GetShapes()) {
                    auto const& indices = s.mesh.indices;
                    for (size_t i = 0; i < s.mesh.material_ids.size(); ++i) {
                        glm::ivec3 verts {
                            indices[3 * i + 0].vertex_index + vert_offset,
                            indices[3 * i + 1].vertex_index + vert_offset,
                            indices[3 * i + 2].vertex_index + vert_offset,
                        };

                        glm::ivec3 norms, uvs;
                        for (int x = 0; x < 3; ++x) {
                            if (indices[3 * i + x].normal_index == -1) {
                                missing_norm = true;
                                norms[x] = -1;
                            } else {
                                norms[x] = indices[3 * i + x].normal_index + norm_offset;
                            }

                            if (indices[3 * i + x].texcoord_index == -1) {
                                missing_uv = true;
                                uvs[x] = -1;
                            } else {
                                uvs[x] = indices[3 * i + x].texcoord_index + uv_offset;
                            }
                        }

                        if (missing_norm) {
                            // deduce normal from cross product
                            glm::vec3 v0v1 = vertices[verts[1]] - vertices[verts[0]];
                            glm::vec3 v0v2 = vertices[verts[2]] - vertices[verts[0]];
                            norms[0] = norms[1] = norms[2] = add_or_get_norm_id(glm::cross(v0v1, v0v2));
                        }

                        int mat_id;
                        if (s.mesh.material_ids[i] >= 0) {
                            mat_id = s.mesh.material_ids[i] + mat_offset;
                        } else {
                            mat_id = -1;
                        }
                        
                        // compute temp buffers for tangent computation
                        if(has_normal_map && !missing_uv) {
                            compute_tan1tan2(verts, uvs);
                            vert2norm[verts[0] - vert_offset] = normals[norms[0]];
                            vert2norm[verts[1] - vert_offset] = normals[norms[1]];
                            vert2norm[verts[2] - vert_offset] = normals[norms[2]];
                        }

                        triangles.emplace_back(verts, norms, uvs, mat_id);
                    }
                }

                if (has_normal_map && !missing_uv) {
                    for (size_t i = 0; i < num_verts; ++i) {
                        Normal const& n = vert2norm[i];
                        glm::vec3 const& t = tan1[i];
                        glm::vec3 const& t2 = tan2[i];
                        // Gram-Schmidt orthogonalize
                        // the 4th component stores handedness
                        tangents.emplace_back(glm::vec4(
                            glm::normalize((t - n * glm::dot(n, t))),
                            glm::dot(glm::cross(n, t), t2) < 0 ? -1.0f : 1.0f
                        ));
                    }

                    auto triangle_it = triangles.begin() + triangles_start;
                    for (auto const& s : reader.GetShapes()) {
                        auto const& indices = s.mesh.indices;
                        for (size_t i = 0; i < s.mesh.material_ids.size(); ++i) {
                            // tangent indices are just verts[0], verts[1], verts[2]
                            // because size of tangent buffer is the num of verts
                            (triangle_it++)->tangents = glm::ivec3(
                                indices[3 * i + 0].vertex_index + tan_offset,
                                indices[3 * i + 1].vertex_index + tan_offset,
                                indices[3 * i + 2].vertex_index + tan_offset
                            );
                        }
                    }
                }

                newGeom.meshid = meshes.size();
                meshes.emplace_back(triangles_start, triangles.size());

                

                cout << dye::green("Loaded:\n")
                    << triangles.size() << " triangles\n"
                    << vertices.size() << " vertices\n"
                    << normals.size() << " normals\n"
                    << uvs.size() << " uvs\n"
                    << meshes.size() << " meshes\n"
                    << tangents.size() << " tangents\n";

                if (missing_uv) {
                    cout << dye::red("missing uv for " + tokens[1]) << endl;
                }
                if (missing_norm) {
                    cout << dye::red("missing norm for " + tokens[1]) << endl;
                }

            } else {
                cerr << "unknown object format" << endl;
                return -1;
            }
        }
    }
    utilityCore::safeGetline(fp_in, line);
    if (!line.empty() && fp_in.good()) {
        vector<string> tokens = utilityCore::tokenizeString(line);
        if (tokens[0] == "material") {
            int mat_id = loadMaterial(tokens[1]);
            if (mat_id < 0) {
                return -1;
            }
            newGeom.materialid = mat_id;
            cout << "Connecting Geom " << objectid << " to Material " << tokens[1] << "..." << endl;
        } else {
            cerr << "unknown field: " << tokens[0] << endl;
            return -1;
        }
    }
    //load transformations
    utilityCore::safeGetline(fp_in, line);
    while (!line.empty() && fp_in.good()) {
        vector<string> tokens = utilityCore::tokenizeString(line);

        //load tranformations
        if (strcmp(tokens[0].c_str(), "TRANS") == 0) {
            newGeom.translation = glm::vec3(atof(tokens[1].c_str()), atof(tokens[2].c_str()), atof(tokens[3].c_str()));
        } else if (strcmp(tokens[0].c_str(), "ROTAT") == 0) {
            newGeom.rotation = glm::vec3(atof(tokens[1].c_str()), atof(tokens[2].c_str()), atof(tokens[3].c_str()));
        } else if (strcmp(tokens[0].c_str(), "SCALE") == 0) {
            newGeom.scale = glm::vec3(atof(tokens[1].c_str()), atof(tokens[2].c_str()), atof(tokens[3].c_str()));
        }

        utilityCore::safeGetline(fp_in, line);
    }

    newGeom.transform = utilityCore::buildTransformationMatrix(
        newGeom.translation, newGeom.rotation, newGeom.scale);
    newGeom.inverseTransform = glm::inverse(newGeom.transform);
    newGeom.invTranspose = glm::inverseTranspose(newGeom.transform);

    // record lights, for now primitives only
    if (newGeom.type != MESH) {
        Material const& mat = materials[newGeom.materialid];
        if (mat.emittance > 0) {
            Light light;
            light.color = mat.diffuse;
            light.intensity = mat.emittance / MAX_EMITTANCE;
            light.position = newGeom.translation;
            lights.emplace_back(light);
        }
    }
    geoms.push_back(newGeom);

    return 1;
}

int Scene::loadCamera() {
    cout << "Loading Camera ..." << endl;
    RenderState &state = this->state;
    Camera &camera = state.camera;
    float fovy;

    //load static properties
    for (int i = 0; i < 5; i++) {
        string line;
        utilityCore::safeGetline(fp_in, line);
        vector<string> tokens = utilityCore::tokenizeString(line);
        if (strcmp(tokens[0].c_str(), "RES") == 0) {
            camera.resolution.x = atoi(tokens[1].c_str());
            camera.resolution.y = atoi(tokens[2].c_str());
        } else if (strcmp(tokens[0].c_str(), "FOVY") == 0) {
            fovy = atof(tokens[1].c_str());
        } else if (strcmp(tokens[0].c_str(), "ITERATIONS") == 0) {
            state.iterations = atoi(tokens[1].c_str());
        } else if (strcmp(tokens[0].c_str(), "DEPTH") == 0) {
            state.traceDepth = atoi(tokens[1].c_str());
        } else if (strcmp(tokens[0].c_str(), "FILE") == 0) {
            state.imageName = tokens[1];
        }
    }

    string line;
    utilityCore::safeGetline(fp_in, line);
    while (!line.empty() && fp_in.good()) {
        vector<string> tokens = utilityCore::tokenizeString(line);
        if (strcmp(tokens[0].c_str(), "EYE") == 0) {
            camera.position = glm::vec3(atof(tokens[1].c_str()), atof(tokens[2].c_str()), atof(tokens[3].c_str()));
        } else if (strcmp(tokens[0].c_str(), "LOOKAT") == 0) {
            camera.lookAt = glm::vec3(atof(tokens[1].c_str()), atof(tokens[2].c_str()), atof(tokens[3].c_str()));
        } else if (strcmp(tokens[0].c_str(), "UP") == 0) {
            camera.up = glm::vec3(atof(tokens[1].c_str()), atof(tokens[2].c_str()), atof(tokens[3].c_str()));
        }

        utilityCore::safeGetline(fp_in, line);
    }

    //calculate fov based on resolution
    float yscaled = tan(fovy * (PI / 180));
    float xscaled = (yscaled * camera.resolution.x) / camera.resolution.y;
    float fovx = (atan(xscaled) * 180) / PI;
    camera.fov = glm::vec2(fovx, fovy);

    camera.right = glm::normalize(glm::cross(camera.view, camera.up));
    camera.pixelLength = glm::vec2(2 * xscaled / (float)camera.resolution.x,
                                   2 * yscaled / (float)camera.resolution.y);

    camera.view = glm::normalize(camera.lookAt - camera.position);

    //set up render camera stuff
    int arraylen = camera.resolution.x * camera.resolution.y;
    state.image.resize(arraylen);
    std::fill(state.image.begin(), state.image.end(), glm::vec3());

    cout << "Loaded camera!" << endl;
    return 1;
}


// standardize material using mtl format
// returns the material id on success or -1 on error
// NOTE: only loads 1 material per mtl file, the rest will be discarded
int Scene::loadMaterial(string mtl_file) {    
    if (mtl_to_id.count(mtl_file)) {
        return mtl_to_id[mtl_file];
    } else {
        ifstream fin(mtl_file);
        if (!fin) {
            cerr << dye::red("cannot find mtl file: ") << dye::red(mtl_file) << endl;
            return -1;
        }

        map<string, int> mat_mp;
        vector<tinyobj::material_t> mats;
        string warn, err;

        tinyobj::LoadMtl(&mat_mp, &mats, &fin, &warn, &err);
        if (!err.empty()) {
            cerr << dye::red("Tiny obj loader: ERROR:\n") << dye::red(err) << endl;
            return -1;
        }
        if (!warn.empty()) {
            cerr << dye::yellow("Tiny obj loader: WARNING:\n") << dye::yellow(warn) << endl;
        }

        if (mat_mp.size() > 1) {
            cerr << dye::yellow("WARNING: ") << dye::yellow(mat_mp.size()-1) << dye::yellow("materials discarded in ") << mtl_file << endl;
        }

        Material material;
        if (!initMaterial(*this, material, mtl_file.substr(0, mtl_file.find_last_of('/')), mats[0])) {
            return -1;
        }
        materials.emplace_back(material);
        
        mtl_to_id[mtl_file] = materials.size() - 1;
        return materials.size() - 1;
    }
}