/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "log.h"

#include "dense_matrix.h"
#include "bulk_operate.h"
#include "mem_dense_matrix.h"
#include "NUMA_dense_matrix.h"
#include "EM_dense_matrix.h"
#include "generic_type.h"
#include "rand_gen.h"
#include "one_val_matrix_store.h"

namespace fm
{

bool dense_matrix::verify_inner_prod(const dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (this->get_entry_size() != left_op.left_entry_size()
			|| m.get_entry_size() != left_op.right_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The left operator isn't compatible with input matrices";
		return false;
	}

	if (left_op.output_entry_size() != right_op.left_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The type of the left operator doesn't match the right operator";
		return false;
	}

	if (right_op.left_entry_size() != right_op.right_entry_size()
			|| right_op.left_entry_size() != right_op.output_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The input and output of the right operator has different types";
		return false;
	}

	if (get_num_cols() != m.get_num_rows()) {
		BOOST_LOG_TRIVIAL(error) << "The matrix size doesn't match";
		return false;
	}
	return true;
}

bool dense_matrix::verify_aggregate(const bulk_operate &op) const
{
	if (op.left_entry_size() != op.right_entry_size()
			|| op.left_entry_size() != op.output_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The input and output type of the operator is different";
		return false;
	}

	if (this->get_entry_size() != op.left_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The matrix entry size is different from the operator";
		return false;
	}
	return true;
}

bool dense_matrix::verify_mapply2(const dense_matrix &m,
			const bulk_operate &op) const
{
	if (this->get_num_rows() != m.get_num_rows()
			|| this->get_num_cols() != m.get_num_cols()) {
		BOOST_LOG_TRIVIAL(error)
			<< "two matrices in mapply2 don't have the same shape";
		return false;
	}

	if (this->store_layout() != m.store_layout()) {
		BOOST_LOG_TRIVIAL(error)
			<< "two matrices in mapply2 don't have the same data layout";
		return false;
	}

	if (get_entry_size() != op.left_entry_size()
			|| m.get_entry_size() != op.right_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "the element type in the matrices isn't compatible with the operator";
		return false;
	}

	return true;
}

bool dense_matrix::verify_apply(apply_margin margin, const arr_apply_operate &op) const
{
	if (get_entry_size() != op.input_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "the element type in the matrices isn't compatible with the operator";
		return false;
	}

	return true;
}

dense_matrix::ptr dense_matrix::create(size_t nrow, size_t ncol,
		const scalar_type &type, matrix_layout_t layout, bool in_mem)
{
	if (in_mem)
		return mem_dense_matrix::create(nrow, ncol, layout, type);
	else {
		return dense_matrix::ptr();
#if 0
		if (layout == matrix_layout_t::L_ROW) {
			fprintf(stderr, "EM row dense matrix isn't supported\n");
			return dense_matrix::ptr();
		}
		else
			return EM_col_dense_matrix::create(nrow, ncol, type);
#endif
	}
}

double dense_matrix::norm2() const
{
	// TODO this is an inefficient implementation.
	dense_matrix::ptr sq_mat = this->mapply2(
			*this, get_type().get_basic_ops().get_multiply());
	scalar_variable::ptr res = sq_mat->aggregate(
			sq_mat->get_type().get_basic_ops().get_add());
	double ret = 0;
	res->get_type().get_basic_uops().get_op(
			basic_uops::op_idx::SQRT)->runA(1, res->get_raw(), &ret);
	return ret;
}

namespace
{

/*
 * This class set elements in a container randomly.
 * set_operate can't change its own state and has to be thread-safe when
 * running on multiple threads. However, random generators aren't
 * thread-safe, so we have to create a random generator for each thread.
 */
class rand_init: public set_operate
{
	class rand_gen_wrapper {
		rand_gen::ptr gen;
	public:
		rand_gen_wrapper(rand_gen::ptr gen) {
			this->gen = gen;
		}

		rand_gen &get_gen() {
			return *gen;
		}
	};

	pthread_key_t gen_key;
	const scalar_type &type;
	const scalar_variable &min;
	const scalar_variable &max;

	rand_gen &get_rand_gen() const {
		void *addr = pthread_getspecific(gen_key);
		if (addr == NULL)
			addr = new rand_gen_wrapper(type.create_rand_gen(min, max));
		rand_gen_wrapper *wrapper = (rand_gen_wrapper *) addr;
		return wrapper->get_gen();
	}

	static void destroy_rand_gen(void *gen) {
		rand_gen_wrapper *wrapper = (rand_gen_wrapper *) gen;
		delete wrapper;
		printf("destroy rand gen\n");
	}
public:
	rand_init(const scalar_variable &_min, const scalar_variable &_max): type(
			_min.get_type()), min(_min), max(_max) {
		int ret = pthread_key_create(&gen_key, destroy_rand_gen);
		assert(ret == 0);
	}

	~rand_init() {
		pthread_key_delete(gen_key);
	}

	virtual void set(void *arr, size_t num_eles, off_t row_idx,
			off_t col_idx) const {
		get_rand_gen().gen(arr, num_eles);
	}
	virtual const scalar_type &get_type() const {
		return get_rand_gen().get_type();
	}
};

}

mem_dense_matrix::ptr mem_dense_matrix::_create_rand(const scalar_variable &min,
		const scalar_variable &max, size_t nrow, size_t ncol,
		matrix_layout_t layout, int num_nodes)
{
	assert(min.get_type() == max.get_type());
	detail::mem_matrix_store::ptr store = detail::mem_matrix_store::create(
			nrow, ncol, layout, min.get_type(), num_nodes);
	store->set_data(rand_init(min, max));
	return mem_dense_matrix::ptr(new mem_dense_matrix(store));
}

mem_dense_matrix::ptr mem_dense_matrix::_create_const(scalar_variable::ptr val,
		size_t nrow, size_t ncol, matrix_layout_t layout, int num_nodes)
{
	detail::mem_matrix_store::ptr store(new detail::one_val_matrix_store(
				val, nrow, ncol, layout));
	return mem_dense_matrix::ptr(new mem_dense_matrix(store));
}

}
