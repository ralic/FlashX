/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include <set>

#include "thread.h"
#include "io_interface.h"

#include "graph_engine.h"
#include "graph_config.h"

const double BIN_SEARCH_RATIO = 100;

atomic_number<long> num_working_vertices;
atomic_number<long> num_completed_vertices;

int timestamp1;
int timestamp2;

class count_msg: public vertex_message
{
	int num;
public:
	count_msg(int num) {
		this->num = num;
	}

	int get_num() const {
		return num;
	}
};

class scan_vertex: public compute_vertex
{
	// The number of vertices that have joined with the vertex.
	int num_joined;
	std::set<vertex_id_t>::const_iterator fetch_it;
	// The number of edges in its neighborhood.
	// in timestamp 1
	atomic_integer num_edges1;
	// in timestamp 2
	atomic_integer num_edges2;
	// All neighbors (in both in-edges and out-edges)
	// neighbors in timestamp 1
	std::set<vertex_id_t> *neighbors1;
	// neighbors in timestamp 2, which are also in timestamp 1.
	std::set<vertex_id_t> *neighbors2;
public:
	scan_vertex(): compute_vertex(-1, -1, 0) {
		num_joined = 0;
		neighbors1 = NULL;
		neighbors2 = NULL;
	}

	scan_vertex(vertex_id_t id, off_t off, int size): compute_vertex(
			id, off, size) {
		num_joined = 0;
		neighbors1 = NULL;
		neighbors2 = NULL;
	}

	virtual bool has_required_vertices() const {
		if (neighbors1 == NULL)
			return false;
		return fetch_it != neighbors1->end();
	}

	virtual vertex_id_t get_next_required_vertex() {
		vertex_id_t id = *fetch_it;
		fetch_it++;
		return id;
	}

	int get_num_edges_diff() const {
		return num_edges1.get() - num_edges2.get();
	}

	int count_edges(const TS_page_vertex *v,
			const std::set<vertex_id_t> *neighbors, int timestamp);

	void run(graph_engine &graph, const page_vertex *vertex);

	void run_on_neighbors(graph_engine &graph,
			const page_vertex *vertices[], int num);

	void run_on_messages(graph_engine &graph,
			const vertex_message *msgs[], int num) {
	}
};

int scan_vertex::count_edges(const TS_page_vertex *v,
		const std::set<vertex_id_t> *neighbors, int timestamp)
{
	int num_local_edges = 0;
	if (v->get_num_edges(timestamp, edge_type::BOTH_EDGES) == 0
			|| neighbors->empty())
		return 0;

	// If the neighbor vertex has way more edges than this vertex.
	if (v->get_num_edges(timestamp,
				edge_type::BOTH_EDGES) / neighbors->size() > BIN_SEARCH_RATIO) {
		page_byte_array::const_iterator<vertex_id_t> other_it
			= v->get_neigh_begin(timestamp, edge_type::BOTH_EDGES);
		page_byte_array::const_iterator<vertex_id_t> other_end
			= v->get_neigh_end(timestamp, edge_type::BOTH_EDGES);
		for (std::set<vertex_id_t>::const_iterator it = neighbors->begin();
				it != neighbors->end(); it++) {
			vertex_id_t this_neighbor = *it;
			// We need to skip loops.
			if (this_neighbor != v->get_id()
					&& this_neighbor != this->get_id()) {
				page_byte_array::const_iterator<vertex_id_t> first
					= std::lower_bound(other_it, other_end, this_neighbor);
				if (first != other_end && this_neighbor == *first) {
					num_local_edges++;
				}
			}
		}
	}
	// If this vertex has way more edges than the neighbor vertex.
	else if (neighbors->size() / v->get_num_edges(timestamp,
				edge_type::BOTH_EDGES) > BIN_SEARCH_RATIO) {
		page_byte_array::const_iterator<vertex_id_t> other_it
			= v->get_neigh_begin(timestamp, edge_type::BOTH_EDGES);
		page_byte_array::const_iterator<vertex_id_t> other_end
			= v->get_neigh_end(timestamp, edge_type::BOTH_EDGES);
		while (other_it != other_end) {
			vertex_id_t neigh_neighbor = *other_it;
			if (neigh_neighbor != v->get_id()
					&& neigh_neighbor != this->get_id()) {
				if (neighbors->find(neigh_neighbor) != neighbors->end()) {
					num_local_edges++;
				}
			}
			++other_it;
		}
	}
	else {
		std::set<vertex_id_t>::const_iterator this_it = neighbors->begin();
		std::set<vertex_id_t>::const_iterator this_end = neighbors->end();
		page_byte_array::const_iterator<vertex_id_t> other_it
			= v->get_neigh_begin(timestamp, edge_type::BOTH_EDGES);
		page_byte_array::const_iterator<vertex_id_t> other_end
			= v->get_neigh_end(timestamp, edge_type::BOTH_EDGES);
		while (this_it != this_end && other_it != other_end) {
			vertex_id_t this_neighbor = *this_it;
			vertex_id_t neigh_neighbor = *other_it;
			if (this_neighbor == neigh_neighbor) {
				// skip loop
				if (neigh_neighbor != v->get_id()
						&& neigh_neighbor != this->get_id()) {
					num_local_edges++;
				}
				++this_it;
				++other_it;
			}
			else if (this_neighbor < neigh_neighbor) {
				++this_it;
			}
			else
				++other_it;
		}
	}
	return num_local_edges;
}

void scan_vertex::run(graph_engine &graph, const page_vertex *vertex)
{
	assert(neighbors1 == NULL);
	assert(neighbors2 == NULL);
	assert(num_joined == 0);

	const TS_page_vertex *ts_vertex = (const TS_page_vertex *) vertex;
	long ret = num_working_vertices.inc(1);
	if (ret % 100000 == 0)
		printf("%ld working vertices\n", ret);
	if (ts_vertex->get_num_edges(timestamp1, edge_type::BOTH_EDGES) == 0) {
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);
		return;
	}

	neighbors1 = new std::set<vertex_id_t>();
	neighbors2 = new std::set<vertex_id_t>();

	page_byte_array::const_iterator<vertex_id_t> it = ts_vertex->get_neigh_begin(
			timestamp1, edge_type::BOTH_EDGES);
	page_byte_array::const_iterator<vertex_id_t> end = ts_vertex->get_neigh_end(
			timestamp1, edge_type::BOTH_EDGES);
	for (; it != end; ++it) {
		vertex_id_t id = *it;
		// Ignore loops
		if (id != ts_vertex->get_id()) {
			neighbors1->insert(id);
		}
	}

	it = ts_vertex->get_neigh_begin(timestamp2, edge_type::BOTH_EDGES);
	end = ts_vertex->get_neigh_end(timestamp2, edge_type::BOTH_EDGES);
	for (; it != end; ++it) {
		vertex_id_t id = *it;
		// Ignore loop
		if (id != ts_vertex->get_id()
				// The neighbor needs to exist in timestamp 1.
				|| neighbors1->find(id) != neighbors1->end())
			neighbors2->insert(id);
	}

	fetch_it = neighbors1->begin();
	num_edges1.inc(neighbors1->size());
	num_edges2.inc(neighbors2->size());
}

void scan_vertex::run_on_neighbors(graph_engine &graph,
		const page_vertex *vertices[], int num)
{
	num_joined++;
	assert(neighbors1);
	assert(neighbors2);
	for (int i = 0; i < num; i++) {
		int ret1 = count_edges((const TS_page_vertex *) vertices[i],
				neighbors1, timestamp1);
		int ret2 = count_edges((const TS_page_vertex *) vertices[i],
				neighbors2, timestamp2);
		// If we find triangles with the neighbor, notify the neighbor
		// as well.
		if (ret1 > 0) {
			num_edges1.inc(ret1);
		}
		if (ret2 > 0) {
			num_edges2.inc(ret2);
		}
	}

	// If we have seen all required neighbors, we have complete
	// the computation. We can release the memory now.
	if (num_joined == (int) neighbors1->size()) {
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);

		delete neighbors1;
		delete neighbors2;
		neighbors1 = NULL;
		neighbors2 = NULL;
	}
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

int main(int argc, char *argv[])
{
	if (argc < 8) {
		fprintf(stderr,
				"scan-statistics conf_file graph_file index_file directed num_timestamps timestamp1 timestamp2 [output_file]\n");
		graph_conf.print_help();
		params.print_help();
		exit(-1);
	}

	std::string conf_file = argv[1];
	std::string graph_file = argv[2];
	std::string index_file = argv[3];
	bool directed = atoi(argv[4]);
	int num_timestamps = atoi(argv[5]);
	timestamp1 = atoi(argv[6]);
	timestamp2 = atoi(argv[7]);
	assert(directed);
	std::string output_file;
	if (argc == 9) {
		output_file = argv[8];
		argc--;
	}

	config_map configs(conf_file);
	configs.add_options(argv + 8, argc - 8);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	graph_index *index = graph_index_impl<scan_vertex>::create(
			index_file, directed);
	graph_engine *graph = graph_engine::create(
			graph_conf.get_num_threads(), params.get_num_nodes(), graph_file,
			index, new ts_ext_mem_vertex_interpreter(num_timestamps), directed);
	// TODO I need to redefine this interface.
	graph->set_required_neighbor_type(edge_type::BOTH_EDGES);
	printf("scan statistics starts\n");
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start_all();
	graph->wait4complete();
	gettimeofday(&end, NULL);

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph->cleanup();
	printf("It takes %f seconds\n", time_diff(start, end));
	printf("There are %ld vertices\n", index->get_num_vertices());
	printf("process %ld vertices and complete %ld vertices\n",
			num_working_vertices.get(), num_completed_vertices.get());

	if (!output_file.empty()) {
		FILE *f = fopen(output_file.c_str(), "w");
		if (f == NULL) {
			perror("fopen");
			return -1;
		}
		std::vector<vertex_id_t> vertices;
		index->get_all_vertices(vertices);
		for (size_t i = 0; i < index->get_num_vertices(); i++) {
			scan_vertex &v = (scan_vertex &) index->get_vertex(vertices[i]);
			fprintf(f, "v%ld: %d\n", v.get_id(), v.get_num_edges_diff());
		}
		fclose(f);
	}
}