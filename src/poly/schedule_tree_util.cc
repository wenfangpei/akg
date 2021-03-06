/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "poly/schedule_tree_util.h"
#include "poly/dma_inject.h"
#include "poly/schedule_pass.h"
#include "isl_schedule_node_private.h"

namespace akg {
namespace ir {
namespace poly {

isl::union_set CollectDomain(const isl::schedule_node &node) {
  int depth = node.get_tree_depth();
  isl::schedule_node tmp_node;
  isl::union_set domain = node.get_domain();
  for (int i = 0; i < depth; ++i) {
    tmp_node = node.ancestor(depth - i);
    if (auto filter_node = tmp_node.as<isl::schedule_node_filter>()) {
      domain = domain.intersect(filter_node.get_filter());
    }
    if (auto extension_node = tmp_node.as<isl::schedule_node_extension>()) {
      auto parent_schedule = ShortSchedule(tmp_node);
      auto extension = extension_node.get_extension();
      parent_schedule = parent_schedule.intersect_domain(domain);
      domain = domain.unite(parent_schedule.range().apply(extension));
    }
  }
  return domain;
}

isl::schedule_node MapDescendantTopDown(isl::schedule_node node,
                                        const std::function<isl::schedule_node(isl::schedule_node)> &fn) {
  unsigned int depth_ = node.get_tree_depth();
  do {
    do {
      node = fn(node);
    } while (node.has_children() && (node = node.first_child()));

    while (node.get_tree_depth() > depth_ && !node.has_next_sibling()) {
      node = node.parent();
    }

    if (node.get_tree_depth() > depth_) {
      node = node.next_sibling();
    }
  } while (node.get_tree_depth() > depth_);

  return node;
}

void GetVisitedStmts(const isl::schedule_node &root) {
  int n = root.n_children();
  if (n <= 0) return;

  isl::schedule_node node;
  if (root.isa<isl::schedule_node_sequence>()) {
    isl::union_set visited_stmts;
    for (int i = 0; i < n; ++i) {
      node = root.child(i);
      auto filter_node = node.as<isl::schedule_node_filter>();
      CHECK(filter_node) << "expected children of sequence to be filters";
      auto filter = filter_node.get_filter().universe();
      if (visited_stmts.get()) {
        CHECK(visited_stmts.intersect(filter).is_empty()) << "filters are expected to be disjoint as stmt level";
        visited_stmts = visited_stmts.unite(filter);
      } else {
        visited_stmts = filter;
      }
    }
  }

  for (int i = 0; i < n; ++i) {
    node = root.child(i);
    GetVisitedStmts(node);
  }
}

std::vector<isl::schedule_node> FilterNode(std::vector<isl::schedule_node> nodes, const std::vector<isl::id> &filters) {
  auto NeedAdd = [](const isl::space &space, const std::vector<isl::id> &filter) -> bool {
    for (const auto &item : filter) {
      if (!space.has_param(item)) {
        return false;
      }
    }
    return true;
  };

  std::vector<isl::schedule_node> res_nodes;
  for (auto node : nodes) {
    if (node.isa<isl::schedule_node_filter>()) {
      auto node_filter = node.as<isl::schedule_node_filter>();
      auto curr_uset = node_filter.filter();
      auto space = curr_uset.get_space();
      if (NeedAdd(space, filters)) {
        res_nodes.push_back(node);
      }
    }
  }
  return res_nodes;
}

isl::schedule_node GenerateEmptyBandInRoot(isl::schedule_node &root) {
  auto node = root;
  if (node.n_children() > 0 && node.child(0).isa<isl::schedule_node_context>()) {
    node = node.child(0).child(0);
  }

  // construct empty band
  isl::space space;
  isl::multi_union_pw_aff mupa;
  auto tmp_domain = node.get_schedule().get_domain();
  space = tmp_domain.get_space().set_from_params();
  auto mv = isl::multi_val::zero(space);
  mupa = isl::multi_union_pw_aff(tmp_domain, mv);

  node = node.insert_partial_schedule(mupa);
  return node;
}

bool ContainsDepth(isl::schedule_node &node, size_t depth) {
  auto depth_before = node.schedule_depth();
  auto band = node.as<isl::schedule_node_band>();
  auto depth_after = depth_before + band.n_member();
  return depth_before < depth && depth_after >= depth;
}

int GetScheduleDepth(isl::schedule &root) {
  int depth = 0;
  auto root_node = root.get_root();
  auto fn = [&depth](const isl::schedule_node &node) -> isl::schedule_node {
    if (node.isa<isl::schedule_node_band>()) {
      auto schedule_depth = static_cast<int>(node.schedule_depth());
      schedule_depth = schedule_depth + static_cast<int>(node.as<isl::schedule_node_band>().n_member());
      depth = schedule_depth > depth ? schedule_depth : depth;
    }
    return node;
  };
  root_node = root_node.map_descendant_bottom_up(fn);
  return depth;
}

std::vector<isl::schedule_node> BandsContainingScheduleDepth(isl::schedule_node &root, size_t depth) {
  if (depth == 0) {
    return {GenerateEmptyBandInRoot(root)};
  }

  std::vector<isl::schedule_node> bands;
  CollectBandsOnTree(root, bands);
  std::function<bool(isl::schedule_node st)> contains_depth = [&depth](isl::schedule_node st) {
    auto depth_before = st.schedule_depth();
    auto band = st.as<isl::schedule_node_band>();
    auto depth_after = depth_before + band.n_member();
    return depth_before < depth && depth_after >= depth;
  };
  return FilterWithFunc(contains_depth, bands);
}

void CollectBandsOnTree(isl::schedule_node &root, std::vector<isl::schedule_node> &bands) {
  for (unsigned int i = 0; i < root.n_children(); ++i) {
    isl::schedule_node node = root.get_child(i);
    if (node.isa<isl::schedule_node_band>()) {
      bands.insert(bands.end(), node);
    }
    CollectBandsOnTree(node, bands);
  }
  return;
}

// whether the node is a "thread_marker".
// It means the band below this node is a thread-mapped band.
bool IsThreadMappedMark(const isl::schedule_node &node) {
  if (node.isa<isl::schedule_node_mark>() && node.n_children() > 0 &&
      node.as<isl::schedule_node_mark>().get_id().get_name().find(THREAD_MARKER) != std::string::npos) {
    return true;
  }
  return false;
}

// find all the ancestors to check whether any of them is a "thread_marker" node.
// NOTE: because of our schedule architecture, the "thread_marker" node is on top
// of thread-mapped band, like:
//  ----------
//  mark: "thread_marker"  <--
//  child:
//     filter : "..."
//     child:
//         schedule: "..." <--
bool IsAncestorMapToThread(const isl::schedule_node &curr_node) {
  bool has_thread_mark_node = false;
  auto FindThreadMarkNode = [&has_thread_mark_node](const isl::schedule_node node) {
    has_thread_mark_node |= IsThreadMappedMark(node);
    return node;
  };
  curr_node.foreach_ancestor_top_down(FindThreadMarkNode);
  return has_thread_mark_node;
}

isl::schedule_node BandSplitAtDepth(isl::schedule_node &band, size_t depth) {
  if (!band.isa<isl::schedule_node_band>()) {
    return band;
  }
  auto n_member = band.as<isl::schedule_node_band>().n_member();
  auto schedule_depth = band.schedule_depth();
  auto depth_after = schedule_depth + n_member;
  return depth_after == depth ? band : band.as<isl::schedule_node_band>().split(depth - schedule_depth);
}

std::vector<isl::schedule_node> BandsSplitAfterDepth(const std::vector<isl::schedule_node> &bands,
                                                     isl::schedule_node &root, size_t depth) {
  std::function<isl::schedule_node(isl::schedule_node)> split_at_depth = [&depth](isl::schedule_node st) {
    auto n_member = st.as<isl::schedule_node_band>().n_member();
    auto schedule_depth = st.schedule_depth();
    auto depth_after = schedule_depth + n_member;
    return depth_after == depth ? st : st.as<isl::schedule_node_band>().split(depth - schedule_depth);
  };
  return MapWithFunc(split_at_depth, bands);
}

std::pair<isl::schedule_node, isl::schedule_node> MapInnerDimToThreads(const isl::schedule_node &node,
                                                                       const bool is_promotion, MappingCfg *mapping_cfg,
                                                                       UpaNodeMapping &upa_node_mapping) {
  CHECK(mapping_cfg != nullptr) << "threadconfig is null";
  isl::schedule_node_band band_node = node.as<isl::schedule_node_band>();
  size_t n_thread_map = std::min(static_cast<size_t>(band_node.n_member()), mapping_cfg->bound);
  CHECK_LE(n_thread_map, mapping_cfg->MaxDim()) << "mapping to too many threads.";

  auto partial_schedule = band_node.get_partial_schedule();
  auto upa_list = partial_schedule.get_union_pw_aff_list().reverse();
  if (is_promotion) {
    // we need to to get range of promoted band from extension node so that we can correctly fix stride
    auto parent = node;
    while (parent && parent.has_parent() && !parent.isa<isl::schedule_node_extension>()) {
      parent = parent.parent();
    }
    if (parent.isa<isl::schedule_node_extension>()) {
      auto extension = parent.as<isl::schedule_node_extension>();
      partial_schedule = partial_schedule.intersect_domain(extension.get_extension().range());
      upa_list = partial_schedule.get_union_pw_aff_list().reverse();
    }
  }
  upa_list = upa_list.drop(n_thread_map, upa_list.size() - n_thread_map);

  isl::schedule_node fix_node = FixStrideInThread(node, upa_list, mapping_cfg);
  bool tiled = !fix_node.is_equal(node);

  // insert node with specific marker
  fix_node = fix_node.insert_mark(isl::id(fix_node.ctx(), THREAD_MARKER));
  fix_node = fix_node.child(0);

  auto after_map_node = CreateAndInsertMapFilter(fix_node, is_promotion, upa_list, mapping_cfg, upa_node_mapping);
  after_map_node = after_map_node.parent();
  if (is_promotion && tiled) {
    after_map_node = after_map_node.parent();
  }

  isl::schedule_node after_fix_node = after_map_node;
  if (tiled && after_fix_node.has_parent()) {
    after_fix_node = after_fix_node.parent();
  }
  return std::make_pair(after_map_node, after_fix_node);
}

isl::schedule_node CreateAndInsertMapFilter(const isl::schedule_node &node, const bool is_promotion,
                                            isl::union_pw_aff_list upa_list, MappingCfg *mapping_cfg,
                                            UpaNodeMapping &upa_node_mapping) {
  // create mapping filter
  CHECK(mapping_cfg != nullptr) << "threadconfig is null";

  Mapping mapping;
  isl::union_set domain = node.get_schedule().get_domain();
  size_t num_map = upa_list.size();
  for (size_t i = 0; i < num_map; ++i) {
    std::pair<std::string, int> cfg = mapping_cfg->GetAt(i);
    auto upa = upa_list.get_at(i);
    CHECK_GT(cfg.second, 0);
    upa = upa.mod(isl::val(node.ctx(), cfg.second));
    auto id = isl::id(node.ctx(), cfg.first);
    mapping[id] = upa;
    domain = upa.domain();
  }
  if (!is_promotion) {
    for (size_t i = num_map; i < mapping_cfg->bound; ++i) {
      CHECK(!domain.is_null());
      auto universe = domain.universe();
      std::pair<std::string, int> cfg = mapping_cfg->GetAt(i);
      auto id = isl::id(node.ctx(), cfg.first);
      mapping[id] = isl::union_pw_aff(universe, isl::val::zero(domain.ctx()));
    }
  }

  // extract unique domain
  auto map_domain = mapping.cbegin()->second.domain();
  for (const auto &kvp : mapping) {
    CHECK(map_domain.is_equal(kvp.second.domain()));
  }
  auto map_filter = map_domain.universe();
  for (const auto &kvp : mapping) {
    auto id = kvp.first;
    auto upa = kvp.second;
    upa = upa.sub(isl::union_pw_aff::param_on_domain(map_domain.universe(), id));
    map_filter = map_filter.intersect(upa.zero_union_set());
  }

  // insert mapping filter
  isl::schedule_node map_filter_node = node;
  map_filter_node = map_filter_node.insert_filter(map_filter);
  upa_node_mapping.emplace_back(std::make_pair(map_filter_node, mapping));
  return map_filter_node;
}

isl::schedule_node FixStrideInThread(const isl::schedule_node &thread_root, const isl::union_pw_aff_list &aff_list,
                                     MappingCfg *mapping_cfg) {
  bool need_fix = false;
  std::vector<int> strides;
  CHECK(mapping_cfg != nullptr) << "threadconfig is null";
  for (size_t i = 0; i < aff_list.size(); ++i) {
    auto aff = aff_list.get_at(i);
    auto extent = aff.max_val().get_num_si() + 1;
    auto stride = mapping_cfg->GetAt(i).second;
    if (extent > stride && extent % stride == 0) {
      need_fix = true;
    }
    strides.emplace_back(stride);
  }
  if (!need_fix) {
    return thread_root;
  }

  isl::multi_val tile_size;
  auto ctx = thread_root.ctx();
  auto space = thread_root.as<isl::schedule_node_band>().get_space();
  tile_size = isl::multi_val::zero(space);

  auto len = static_cast<int>(strides.size());
  for (auto i = len - 1; i >= 0; --i) {
    tile_size = tile_size.set_val(len - 1 - i, isl::val(ctx, strides[i]));
  }

  isl::schedule_node after_tile_node = TileBand(thread_root, tile_size);
  return after_tile_node.child(0);
}

bool IsEqual(const isl::schedule_node node1, const isl::schedule_node node2) {
  auto node_ptr1 = node1.get();
  auto node_ptr2 = node2.get();

  if (!node_ptr1 || !node_ptr2) {
    return false;
  }
  if (node_ptr1 == node_ptr2) {
    return true;
  }

  auto n1 = isl_schedule_node_get_tree_depth(node_ptr1);
  auto n2 = isl_schedule_node_get_tree_depth(node_ptr2);
  if (n1 < 0 || n2 < 0) {
    return false;
  }
  if (n1 != n2) {
    return false;
  }
  for (int i = 0; i < n1; ++i) {
    if (node_ptr1->child_pos[i] != node_ptr2->child_pos[i]) {
      return false;
    }
  }

  return true;
}

isl::multi_union_pw_aff MapDomainToThread(const isl::schedule_node &node, MappingCfg *mapping_cfg,
                                          const UpaNodeMapping &upa_node_mapping) {
  std::vector<isl::id> thread_ids;
  for (size_t i = 0; i < mapping_cfg->bound; ++i) {
    auto ti = mapping_cfg->GetAt(i);
    auto id = isl::id(node.ctx(), ti.first);
    thread_ids.emplace_back(id);
  }

  isl::space space = isl::space(node.ctx(), 0);
  isl::union_set empty_domain = isl::union_set::empty(space);
  space = space.add_named_tuple_id_ui(isl::id(node.ctx(), SYNC_BLOCK), thread_ids.size());
  auto domain_threads = isl::multi_union_pw_aff(empty_domain, isl::multi_val::zero(space));

  for (auto upa_mapping : upa_node_mapping) {
    auto upa_node = upa_mapping.first;
    auto tmp_node = upa_node;
    while (!tmp_node.is_null() && tmp_node.has_parent() && !tmp_node.isa<isl::schedule_node_mark>()) {
      tmp_node = tmp_node.parent();
    }

    if (tmp_node.isa<isl::schedule_node_mark>()) {
      std::string mark_id = tmp_node.as<isl::schedule_node_mark>().get_id().get_name();
      if (mark_id.find(THREAD_MARKER) == std::string::npos) {
        continue;
      }
    }

    if (!tmp_node.is_null() && tmp_node.has_parent()) {
      tmp_node = tmp_node.parent();
    }

    for (size_t i = 0; i < node.n_children(); ++i) {
      auto node_child = node.child(i);
      if (IsEqual(tmp_node, node_child)) {
        auto mapping = upa_mapping.second;
        auto upa_list = isl::union_pw_aff_list(node.ctx(), thread_ids.size());
        for (auto thread_id : thread_ids) {
          if (mapping.count(thread_id) == 0) {
            break;
          }
          upa_list = upa_list.add(mapping.at(thread_id));
        }
        if (upa_list.size() == thread_ids.size()) {
          auto domain_upa_node = CollectDomain(upa_node);
          auto domain_intersection = domain_upa_node.intersect(domain_threads.domain());
          CHECK(domain_intersection.is_empty())
            << "This domain has been mapped to threadID and show that there is an intersection.";

          auto upa_node_thread = isl::multi_union_pw_aff(space, upa_list);
          upa_node_thread = upa_node_thread.intersect_domain(domain_upa_node);
          domain_threads = domain_threads.union_add(upa_node_thread);
        }
      }
    }
  }

  auto domain_node = CollectDomain(node);
  bool sub_set = domain_node.is_subset(domain_threads.domain());
  CHECK(sub_set) << "There are remaining domains that have not been mapped to threadID";

  return domain_threads;
}

// this function map domain from all the mapping with specific marker: thread_marker or block_marker.
// we foreach the upa_node_mapping and check whether a mapping belongs to thread/block marker.
isl::multi_union_pw_aff MapDomainAllWithType(const isl::schedule_node &node, MappingCfg *mapping_cfg,
                                             const UpaNodeMapping &upa_node_mapping, const std::string map_type) {
  CHECK((map_type == THREAD_MARKER || map_type == BLOCK_MARKER)) << "map_type should be THREAD_MARKER or BLCOK_MARKER.";
  std::vector<isl::id> ids;
  for (size_t i = 0; i < mapping_cfg->bound; ++i) {
    auto ti = mapping_cfg->GetAt(i);
    auto id = isl::id(node.ctx(), ti.first);
    ids.emplace_back(id);
  }

  isl::space space = isl::space(node.ctx(), 0);
  isl::union_set empty_domain = isl::union_set::empty(space);
  space = space.add_named_tuple_id_ui(isl::id(node.ctx(), map_type), ids.size());
  // domain_association: connect thread/block with domain
  auto domain_association = isl::multi_union_pw_aff(empty_domain, isl::multi_val::zero(space));

  for (auto upa_mapping : upa_node_mapping) {
    auto upa_node = upa_mapping.first;
    auto tmp_node = upa_node;
    CHECK(!tmp_node.is_null() && tmp_node.has_parent()) << "node from upa_node_mapping is invalid.";
    // get the marker node above this filter node.
    tmp_node = tmp_node.parent();
    // check whether this node is a mark node with map_type.
    if (!tmp_node.isa<isl::schedule_node_mark>() ||
        (tmp_node.isa<isl::schedule_node_mark>() &&
         tmp_node.as<isl::schedule_node_mark>().get_id().get_name().find(map_type) == std::string::npos)) {
      continue;
    }

    auto mapping = upa_mapping.second;
    auto upa_list = isl::union_pw_aff_list(node.ctx(), ids.size());
    for (auto id : ids) {
      if (mapping.count(id) == 0) {
        break;
      }
      upa_list = upa_list.add(mapping.at(id));
    }
    if (upa_list.size() == ids.size()) {
      auto domain_upa_node = CollectDomain(upa_node);
      auto domain_intersection = domain_upa_node.intersect(domain_association.domain());
      CHECK(domain_intersection.is_empty())
        << "This domain has been mapped to threadID/blockID and show that there is an intersection.";

      auto upa_node_association = isl::multi_union_pw_aff(space, upa_list);
      upa_node_association = upa_node_association.intersect_domain(domain_upa_node);
      domain_association = domain_association.union_add(upa_node_association);
    }
  }

  auto domain_node = CollectDomain(node);
  bool sub_set = domain_node.is_subset(domain_association.domain());
  CHECK(sub_set) << "There are remaining domains that have not been mapped to threadID/blockID";

  return domain_association;
}

isl::map CreateMapIncreaseDim(isl::space space, unsigned dim) {
  isl::space map_space = space.map_from_set();
  isl::multi_aff identity = isl::multi_aff::identity(map_space);

  if (dim < 0 || dim >= identity.size()) {
    LOG(FATAL) << "In the space, " << dim << " should be in the range of [0, " << identity.size() << ")";
  }

  isl::aff aff = identity.get_aff(dim);
  identity = identity.set_aff(dim, aff + 1);
  return isl::map(identity);
}

std::vector<isl::schedule_node> CollectFnNode(const std::function<bool(const isl::schedule_node &)> &fn,
                                              const isl::schedule_node &root) {
  std::vector<isl::schedule_node> res_nodes;
  auto GetFnNode = [&res_nodes, &fn](isl::schedule_node node) -> isl::schedule_node {
    if (fn(node)) {
      res_nodes.push_back(node);
    }
    return node;
  };
  root.map_descendant_bottom_up(GetFnNode);
  return res_nodes;
}

isl::val GetInstancesBound(isl::schedule_node &node, isl::union_map ancestors_schedule, isl::val unroll_val) {
  auto instances_bound = isl::val::zero(unroll_val.ctx());
  if (!node.has_children()) {
    instances_bound = isl::val::one(unroll_val.ctx());
  } else {
    // Combine the schedule of ancestors and expand own schedule.
    auto next_schedule = ancestors_schedule;
    if (auto band_node = node.as<isl::schedule_node_band>()) {
      if (band_node.n_member() > 0) {
        next_schedule = next_schedule.flat_range_product(band_node.get_partial_schedule_union_map());
      }
    } else if (auto filter_node = node.as<isl::schedule_node_filter>()) {
      next_schedule = next_schedule.intersect_domain(filter_node.get_filter());
    } else if (auto extension_node = node.as<isl::schedule_node_extension>()) {
      next_schedule =
        next_schedule.unite(extension_node.get_extension().reverse().intersect_range(next_schedule.range()));
    }

    for (size_t i = 0; i < node.n_children(); ++i) {
      auto child = node.child(i);
      instances_bound = instances_bound.add(GetInstancesBound(child, next_schedule, unroll_val));
      node = child.parent();
    }
  }

  // Calculate the total bound of instances executed by this band node.
  if (auto band_node = node.as<isl::schedule_node_band>()) {
    if (instances_bound.gt(unroll_val)) {
      return isl::val::infty(unroll_val.ctx());
    }

    isl::multi_union_pw_aff partial_schedule = band_node.get_partial_schedule();
    isl::union_pw_aff_list upa_list = partial_schedule.get_union_pw_aff_list();
    isl::space space = partial_schedule.get_space().params();

    for (size_t i = 0; i < band_node.n_member(); ++i) {
      isl::union_pw_aff upa = partial_schedule.get_at(i);
      auto tmp_scheduel = ancestors_schedule;
      if (i != band_node.n_member()-1) {
        upa_list = upa_list.drop(i, 1);
        isl::space unnamed_space = space.add_unnamed_tuple_ui(upa_list.size());
        auto unname_upa = isl::multi_union_pw_aff(unnamed_space, upa_list);
        tmp_scheduel = tmp_scheduel.flat_range_product(isl::union_map::from(unname_upa));
      }
      // For fixed values of ancestors_schedule, calculate a bound on the range of values attained by upa.
      auto union_map = isl::union_map::from(isl::multi_union_pw_aff(upa)).apply_domain(tmp_scheduel);
      isl::val upa_bound = isl::val::zero(upa.ctx());
      if (!union_map.is_empty()) {
        union_map = union_map.range_product(union_map).range().unwrap().project_out_all_params();

        isl::set delta = isl::map::from(union_map).deltas();
        isl::basic_set hull = delta.simple_hull();
        isl::val stride = isl::set(hull).get_stride(0);
        hull = isl::set(hull).polyhedral_hull();

        upa_bound = hull.dim_max_val(0);
        upa_bound = upa_bound.div(stride).add(isl::val::one(upa.ctx()));
      }
      instances_bound = instances_bound.mul(upa_bound);
      if (instances_bound.gt(unroll_val)) {
        return isl::val::infty(unroll_val.ctx());
      }
      band_node = band_node.member_set_ast_loop_unroll(i);
      node = band_node;
    }
  }
  return instances_bound;
}

isl::schedule_node UnrollByMarkOptions(isl::schedule_node &node, uint64_t unroll) {
  if (unroll <= 1) {
    return node;
  }

  int depth = node.get_tree_depth();
  isl::schedule_node tmp_node;
  isl::union_set domain = node.get_schedule().get_domain();

  // In the mapping, locate above the mark to get the corresponding domain.
  auto child_node = node;
  if (node.isa<isl::schedule_node_mark>() && node.has_children()) {
    child_node = node.child(0);
  }
  for (int i = 0; i < depth; ++i) {
    tmp_node = child_node.ancestor(depth - i);

    if (tmp_node.isa<isl::schedule_node_mark>()) {
      std::string mark_id = tmp_node.as<isl::schedule_node_mark>().get_id().get_name();
      if (mark_id.find(THREAD_MARKER) != std::string::npos) {
        if (tmp_node.has_children()) {
          if (auto filter_node = tmp_node.child(0).as<isl::schedule_node_filter>()) {
            domain = domain.intersect(filter_node.get_filter());
          }
        }
      }
    }

    if (auto extension_node = tmp_node.as<isl::schedule_node_extension>()) {
      auto parent_schedule = ShortSchedule(tmp_node);
      auto extension = extension_node.get_extension();
      parent_schedule = parent_schedule.intersect_domain(domain);
      domain = domain.unite(parent_schedule.range().apply(extension));
    }
  }

  auto unroll_val = isl::val(node.ctx(), unroll);
  auto ancestors_schedule = ShortSchedule(node);

  ancestors_schedule = ancestors_schedule.intersect_domain(domain);
  GetInstancesBound(node, ancestors_schedule, unroll_val);
  return node;
}

}  // namespace poly
}  // namespace ir
}  // namespace akg
