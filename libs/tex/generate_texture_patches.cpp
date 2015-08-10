#include "texturing.h"
#include <Eigen/Sparse>

TEX_NAMESPACE_BEGIN

void merge_vertex_projection_infos(std::vector<std::vector<VertexProjectionInfo> > * vertex_projection_infos) {
    /* Merge vertex infos within the same texture patch. */
    #pragma omp parallel for
    for (std::size_t i = 0; i < vertex_projection_infos->size(); ++i) {
        std::vector<VertexProjectionInfo> * infos = &(vertex_projection_infos->at(i));

        std::map<std::size_t, VertexProjectionInfo> info_map;
        std::map<std::size_t, VertexProjectionInfo>::iterator it;

        for (VertexProjectionInfo const & info : *infos) {
            std::size_t texture_patch_id = info.texture_patch_id;
            if((it = info_map.find(texture_patch_id)) == info_map.end()) {
                info_map[texture_patch_id] = info;
            } else {
                it->second.faces.insert(it->second.faces.end(),
                    info.faces.begin(), info.faces.end());
            }
        }

        infos->clear();
        infos->reserve(info_map.size());
        for (it = info_map.begin(); it != info_map.end(); ++it) {
            infos->push_back(it->second);
        }
    }
}

/** Struct representing a TexturePatch candidate - final texture patches are obtained by merging candiates. */
struct TexturePatchCandidate {
    Rect<int> bounding_box;
    TexturePatch texture_patch;
};

/** Create a TexturePatchCandidate by calculating the faces' bounding box projected into the view,
  *  relative texture coordinates and extacting the texture views relevant part
  */
TexturePatchCandidate
generate_candidate(int label, TextureView const & texture_view,
    std::vector<std::size_t> const & faces, mve::TriangleMesh::ConstPtr mesh) {

    mve::ByteImage::Ptr view_image = texture_view.get_image();
    int min_x = view_image->width(), min_y = view_image->height();
    int max_x = 0, max_y = 0;

    mve::TriangleMesh::FaceList const & mesh_faces = mesh->get_faces();
    mve::TriangleMesh::VertexList const & vertices = mesh->get_vertices();

    std::vector<math::Vec2f> texcoords;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            math::Vec3f vertex = vertices[mesh_faces[faces[i] * 3 + j]];
            math::Vec2f pixel = texture_view.get_pixel_coords(vertex);

            texcoords.push_back(pixel);

            min_x = std::min(static_cast<int>(std::floor(pixel[0])) - texture_patch_border, min_x);
            min_y = std::min(static_cast<int>(std::floor(pixel[1])) - texture_patch_border, min_y);
            max_x = std::max(static_cast<int>(std::ceil(pixel[0])) + texture_patch_border, max_x);
            max_y = std::max(static_cast<int>(std::ceil(pixel[1])) + texture_patch_border, max_y);
        }
    }

    /* Calculate the relative texcoords. */
    math::Vec2f min(min_x, min_y);
    for (std::size_t i = 0; i < texcoords.size(); ++i) {
        texcoords[i] = texcoords[i] - min;
    }

    int const width = max_x - min_x;
    int const height = max_y - min_y;

    /* Check for valid projections/erroneous labeling files. */
    assert(min_x >= 0 - texture_patch_border);
    assert(min_y >= 0 - texture_patch_border);
    assert(max_x < view_image->width() + texture_patch_border);
    assert(max_y < view_image->height() + texture_patch_border);

    mve::ByteImage::Ptr image;
    image = mve::image::crop(view_image, width, height, min_x, min_y, *math::Vec3uc(255, 0, 255));
    mve::image::gamma_correct(image, 2.2f);

    TexturePatchCandidate texture_patch_candidate =
        {Rect<int>(min_x, min_y, max_x, max_y), TexturePatch(label, faces, texcoords, image)};
    return texture_patch_candidate;
}

void
generate_texture_patches(UniGraph const & graph, std::vector<TextureView> const & texture_views,
    mve::TriangleMesh::ConstPtr mesh, mve::VertexInfoList::ConstPtr vertex_infos,
    std::vector<std::vector<VertexProjectionInfo> > * vertex_projection_infos,
    std::vector<TexturePatch> * texture_patches) {

    util::WallTimer timer;

    mve::TriangleMesh::FaceList const & mesh_faces = mesh->get_faces();
    mve::TriangleMesh::VertexList const & vertices = mesh->get_vertices();
    vertex_projection_infos->resize(vertices.size());

    std::size_t num_patches = 0;

    std::cout << "\tRunning... " << std::flush;
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t i = 0; i < texture_views.size(); ++i) {

        std::vector<std::vector<std::size_t> > subgraphs;
        int const label = i + 1;
        graph.get_subgraphs(label, &subgraphs);

        std::list<TexturePatchCandidate> candidates;
        for (std::size_t j = 0; j < subgraphs.size(); ++j) {
            candidates.push_back(generate_candidate(label, texture_views[i], subgraphs[j], mesh));
        }

        /* Merge candidates which contain the same image content. */
        std::list<TexturePatchCandidate>::iterator it, sit;
        for (it = candidates.begin(); it != candidates.end(); ++it) {
            for (sit = candidates.begin(); sit != candidates.end();) {
                Rect<int> bounding_box = sit->bounding_box;
                if (it != sit && bounding_box.is_inside(&it->bounding_box)) {
                    TexturePatch::Faces & faces = it->texture_patch.get_faces();
                    TexturePatch::Faces & ofaces = sit->texture_patch.get_faces();
                    faces.insert(faces.end(), ofaces.begin(), ofaces.end());

                    TexturePatch::Texcoords & texcoords = it->texture_patch.get_texcoords();
                    TexturePatch::Texcoords & otexcoords = sit->texture_patch.get_texcoords();
                    math::Vec2f offset;
                    offset[0] = sit->bounding_box.min_x - it->bounding_box.min_x;
                    offset[1] = sit->bounding_box.min_y - it->bounding_box.min_y;
                    for (std::size_t i = 0; i < otexcoords.size(); ++i) {
                        texcoords.push_back(otexcoords[i] + offset);
                    }

                    sit = candidates.erase(sit);
                } else {
                    ++sit;
                }
            }
        }

        it = candidates.begin();
        for (; it != candidates.end(); ++it) {
            std::size_t texture_patch_id;

            #pragma omp critical
            {
                texture_patches->push_back(it->texture_patch);
                texture_patch_id = num_patches++;
            }

            std::vector<std::size_t> const & faces = it->texture_patch.get_faces();
            std::vector<math::Vec2f> const & texcoords = it->texture_patch.get_texcoords();
            for (std::size_t i = 0; i < faces.size(); ++i) {
                std::size_t const face_id = faces[i];
                std::size_t const face_pos = face_id * 3;
                for (std::size_t j = 0; j < 3; ++j) {
                    std::size_t const vertex_id = mesh_faces[face_pos  + j];
                    math::Vec2f const projection = texcoords[i * 3 + j];

                    VertexProjectionInfo info = {texture_patch_id, projection, {face_id}};

                    #pragma omp critical
                    vertex_projection_infos->at(vertex_id).push_back(info);
                }
            }
        }
    }


    std::size_t num_holes = 0;

    //if (!settings.skip_hole_filling) {
    {
        std::vector<std::vector<std::size_t> > subgraphs;
        graph.get_subgraphs(0, &subgraphs);

        #pragma omp parallel for
        for (std::size_t i = 0; i < subgraphs.size(); ++i) {
            std::vector<std::size_t> const & subgraph = subgraphs[i];

            std::map<std::size_t, std::set<std::size_t> > tmp;

            for (std::size_t const face_id : subgraph) {
                std::size_t const v0 = mesh_faces[face_id * 3];
                std::size_t const v1 = mesh_faces[face_id * 3 + 1];
                std::size_t const v2 = mesh_faces[face_id * 3 + 2];
                tmp[v0].insert({v1, v2});
                tmp[v1].insert({v0, v2});
                tmp[v2].insert({v0, v1});
            }

            std::size_t const num_vertices = tmp.size();
            /* Only fill small holes. */
            if (num_vertices > 100) continue;


            /* Calculate 2D parameterization using the technique from libremesh/patch2d,
             * which was published as sourcecode accompanying the following paper:
             *
             * Isotropic Surface Remeshing
             * Simon Fuhrmann, Jens Ackermann, Thomas Kalbe, Michael Goesele
             */

            std::size_t seed = -1;
            std::vector<bool> is_border(num_vertices, false);
            std::vector<std::vector<std::size_t> > new_adj_verts(num_vertices);
            /* Index structures to map from local <-> global vertex id. */
            std::map<std::size_t, std::size_t> g2l;
            std::vector<std::size_t> l2g(num_vertices);
            /* Index structure to determine column in matrix/vector. */
            std::vector<std::size_t> idx(num_vertices);

            std::size_t num_border_vertices = 0;

            bool disk_topology = true;
            std::map<std::size_t, std::set<std::size_t> >::iterator it = tmp.begin();
            for (std::size_t j = 0; j < num_vertices; ++j, ++it) {
                std::size_t vertex_id = it->first;
                g2l[vertex_id] = j;
                l2g[j] = vertex_id;
                new_adj_verts[j] = std::vector<std::size_t>(it->second.begin(), it->second.end());

                /* Check topology in original mesh. */
                if (vertex_infos->at(vertex_id).vclass != mve::VERTEX_CLASS_SIMPLE) {
                    disk_topology = false;
                    break;
                }

                /* Check new topology and determine if vertex is now at the border. */
                std::vector<std::size_t> const & adj_faces = vertex_infos->at(vertex_id).faces;
                std::vector<std::pair<std::size_t, std::size_t> > fan;
                for (std::size_t k = 0; k < adj_faces.size(); ++k) {
                    if (graph.get_label(adj_faces[k]) == 0) {
                        std::size_t curr = adj_faces[k];
                        std::size_t next = adj_faces[(k + 1) % adj_faces.size()];
                        std::pair<std::size_t, std::size_t> pair(curr, next);
                        fan.push_back(pair);
                    }
                }

                std::size_t gaps = 0;
                for (std::size_t k = 0; k < fan.size(); k++) {
                    if (fan[k].second != fan[(k + 1) % fan.size()].first) {
                        ++gaps;
                    }
                }

                is_border[j] = gaps == 1;

                /* Check if vertex is now complex. */
                if (gaps > 1) {
                    disk_topology = false;
                    break;
                }

                if (is_border[j]) {
                    idx[j] = num_border_vertices++;
                    seed = vertex_id;
                } else {
                    idx[j] = j - num_border_vertices;
                }
            }

            if (!disk_topology) continue;
            if (num_border_vertices == 0) {
                //std::cerr << "Isolated object" << std::endl;
                continue;
            }

            std::vector<std::size_t> border; border.reserve(num_vertices);
            std::size_t prev = seed;
            std::size_t curr = seed;
            while (prev == seed || curr != seed) {
                std::size_t next = std::numeric_limits<std::size_t>::max();
                std::vector<std::size_t> const & adj_verts = new_adj_verts[g2l[curr]];
                for (std::size_t adj_vert : adj_verts) {
                    if (is_border[g2l[adj_vert]]
                        && adj_vert != prev && adj_vert != curr) {
                        next = adj_vert;
                    }
                }
                if (next != std::numeric_limits<std::size_t>::max()) {
                    prev = curr;
                    curr = next;
                    border.push_back(next);
                } else {
                    //std::cerr << "No new border vertex" << std::endl;
                    break;
                }
            }

            if (border.size() != num_border_vertices) {
                //std::cerr << "Unclosed border" << std::endl;
                continue;
            }

            float total_length = 0.0f;
            for (std::size_t j = 0; j < border.size(); ++j) {
                math::Vec3f const & v0 = vertices[border[j]];
                math::Vec3f const & v1 = vertices[border[(j + 1) % border.size()]];
                total_length += (v0 - v1).norm();
            }
            float length = 0.0f;
            std::vector<math::Vec2f> projection(num_vertices);
            for (std::size_t j = 0; j < border.size(); ++j) {
                float angle = 2.0f * MATH_PI * length / total_length;
                projection[g2l[border[j]]] = math::Vec2f(std::cos(angle), std::sin(angle));
                math::Vec3f const & v0 = vertices[border[j]];
                math::Vec3f const & v1 = vertices[border[(j + 1) % border.size()]];
                length += (v0 - v1).norm();
            }

            typedef Eigen::Triplet<float, int> Triplet;
            std::vector<Triplet> coeff;
            std::size_t matrix_size = num_vertices - border.size();

            Eigen::VectorXf xx(matrix_size), xy(matrix_size);

            if (matrix_size != 0) {
                Eigen::VectorXf bx(matrix_size);
                Eigen::VectorXf by(matrix_size);
                for (std::size_t j = 0; j < num_vertices; ++j) {
                    if (is_border[j]) continue;

                    std::size_t const vertex_id = l2g[j];

                    /* Calculate "Mean Value Coordinates" as proposed by Michael S. Floater */
                    std::map<std::size_t, float> weights;
                    std::vector<std::size_t> const & adj_faces = vertex_infos->at(vertex_id).faces;
                    for (std::size_t adj_face : adj_faces) {
                        std::size_t v0 = mesh_faces[adj_face * 3];
                        std::size_t v1 = mesh_faces[adj_face * 3 + 1];
                        std::size_t v2 = mesh_faces[adj_face * 3 + 2];
                        if (v1 == vertex_id) std::swap(v1, v0);
                        if (v2 == vertex_id) std::swap(v2, v0);

                        math::Vec3f v01 = vertices[v1] - vertices[v0];
                        float v01n = v01.norm();
                        math::Vec3f v02 = vertices[v2] - vertices[v0];
                        float v02n = v02.norm();
                        float alpha = std::acos(v01.dot(v02) / (v01n * v02n));
                        weights[g2l[v1]] += std::tan(alpha / 2.0f) / v01n;
                        weights[g2l[v2]] += std::tan(alpha / 2.0f) / v02n;
                    }

                    std::map<std::size_t, float>::iterator it;
                    float sum = 0.0f;
                    for (it = weights.begin(); it != weights.end(); ++it)
                        sum += it->second;
                    for (it = weights.begin(); it != weights.end(); ++it)
                        it->second /= sum;

                    bx[idx[j]] = 0.0f;
                    by[idx[j]] = 0.0f;
                    for (it = weights.begin(); it != weights.end(); ++it) {
                        if (is_border[it->first]) {
                            std::size_t border_vertex_id = border[idx[it->first]];
                            bx[idx[j]] += projection[g2l[border_vertex_id]][0] * it->second;
                            by[idx[j]] += projection[g2l[border_vertex_id]][1] * it->second;
                        } else {
                            coeff.push_back(Triplet(idx[j], idx[it->first], -it->second));
                        }
                    }
                }
                for (std::size_t j = 0; j < matrix_size; ++j) {
                    coeff.push_back(Triplet(j, j, 1.0f));
                }

                typedef Eigen::SparseMatrix<float> SpMat;
                SpMat A(matrix_size, matrix_size);
                A.setFromTriplets(coeff.begin(), coeff.end());

                Eigen::SparseLU<SpMat> solver;
                solver.analyzePattern(A);
                solver.factorize(A);
                xx = solver.solve(bx);
                xy = solver.solve(by);
            }

            int image_size = 100; //TODO better estimate
            int scale = image_size / 2 - texture_patch_border;
            for (std::size_t j = 0, k = 0; j < num_vertices; ++j) {
                if (!is_border[j]) {
                    projection[j] = math::Vec2f(xx[k], xy[k]) * scale + image_size / 2;
                    ++k;
                } else {
                    projection[j] = projection[j] * scale + image_size / 2;
                }
            }

            mve::ByteImage::Ptr image = mve::ByteImage::create(image_size, image_size, 3);
            std::vector<math::Vec2f> texcoords; texcoords.reserve(subgraph.size());
            for (std::size_t const face_id : subgraph) {
                for (std::size_t j = 0; j < 3; ++j) {
                    std::size_t const vertex_id = mesh_faces[face_id * 3 + j];
                    texcoords.push_back(projection[g2l[vertex_id]]);
                }
            }
            TexturePatch texture_patch(0, subgraph, texcoords, image);
            std::size_t texture_patch_id;
            #pragma omp critical
            {
                texture_patches->push_back(texture_patch);
                texture_patch_id = num_patches++;
            }

            ++num_holes;

            for (std::size_t j = 0; j < num_vertices; ++j) {
                std::size_t const vertex_id = l2g[j];
                std::vector<std::size_t> const & adj_faces = vertex_infos->at(vertex_id).faces;
                std::vector<std::size_t> faces; faces.reserve(adj_faces.size());
                for (std::size_t adj_face : adj_faces) {
                    if (graph.get_label(adj_face) == 0) {
                        faces.push_back(adj_face);
                    }
                }
                VertexProjectionInfo info = {texture_patch_id, projection[j], faces};
                #pragma omp critical
                vertex_projection_infos->at(vertex_id).push_back(info);
            }
        }
    }

    merge_vertex_projection_infos(vertex_projection_infos);

    std::cout << "done. (Took " << timer.get_elapsed_sec() << "s)" << std::endl;
    std::cout << "\t" << num_patches << " texture patches." << std::endl;
    std::cout << "\t" << num_holes << " holes." << std::endl;
}

TEX_NAMESPACE_END
