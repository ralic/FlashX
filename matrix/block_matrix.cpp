/*
 * Copyright 2016 Open Connectome Project (http://openconnecto.me)
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

#include "block_matrix.h"
#include "vector.h"
#include "local_matrix_store.h"

namespace fm
{

dense_matrix::ptr block_matrix::create(
		detail::combined_matrix_store::const_ptr store)
{
	if (store->get_mat_ref(0).is_wide()) {
		for (size_t i = 1; i < store->get_num_mats() - 1; i++)
			if (store->get_mat_ref(i).get_num_rows()
					!= store->get_mat_ref(i - 1).get_num_rows()) {
				BOOST_LOG_TRIVIAL(error)
					<< "The matrices have different block sizes";
				return dense_matrix::ptr();
			}
	}
	else {
		for (size_t i = 1; i < store->get_num_mats() - 1; i++)
			if (store->get_mat_ref(i).get_num_cols()
					!= store->get_mat_ref(i - 1).get_num_cols()) {
				BOOST_LOG_TRIVIAL(error)
					<< "The matrices have different block sizes";
				return dense_matrix::ptr();
			}
	}
	return dense_matrix::ptr(new block_matrix(store));
}

dense_matrix::ptr block_matrix::create(size_t num_rows, size_t num_cols,
		size_t block_size, const scalar_type &type, const set_operate &op,
		int num_nodes, bool in_mem, safs::safs_file_group::ptr group)
{
	// For tall matrices
	if (num_rows > num_cols) {
		// If there is only one block.
		if (num_cols < block_size)
			return dense_matrix::create(num_rows, num_cols,
					matrix_layout_t::L_COL, type, op, num_nodes, in_mem, group);

		std::vector<detail::matrix_store::ptr> stores(div_ceil<size_t>(
					num_cols, block_size));
		for (size_t i = 0; i < stores.size(); i++) {
			size_t local_num_cols = std::min(num_cols - i * block_size,
					block_size);
			stores[i] = detail::matrix_store::create(num_rows, local_num_cols,
					matrix_layout_t::L_COL, type, num_nodes, in_mem, group);
			// TODO we lose some information if we initialize them individually.
			stores[i]->set_data(op);
		}
		std::vector<detail::matrix_store::const_ptr> const_stores(
				stores.begin(), stores.end());
		return block_matrix::create(detail::combined_matrix_store::create(
					const_stores, matrix_layout_t::L_COL));
	}
	else {
		// If there is only one block.
		if (num_rows < block_size)
			return dense_matrix::create(num_rows, num_cols,
					matrix_layout_t::L_ROW, type, op, num_nodes, in_mem, group);

		std::vector<detail::matrix_store::ptr> stores(div_ceil<size_t>(
					num_rows, block_size));
		for (size_t i = 0; i < stores.size(); i++) {
			size_t local_num_rows = std::min(num_rows - i * block_size,
					block_size);
			stores[i] = detail::matrix_store::create(local_num_rows, num_cols,
					matrix_layout_t::L_ROW, type, num_nodes, in_mem, group);
			// TODO we lose some information if we initialize them individually.
			stores[i]->set_data(op);
		}
		std::vector<detail::matrix_store::const_ptr> const_stores(
				stores.begin(), stores.end());
		return block_matrix::create(detail::combined_matrix_store::create(
					const_stores, matrix_layout_t::L_ROW));
	}
}

matrix_layout_t block_matrix::store_layout() const
{
	// All matrices in the group should have the same data layout.
	return store->get_mat_ref(0).store_layout();
}

bool block_matrix::is_virtual() const
{
	// If one matrix is virtual, all other matrices should also be virtual.
	return store->get_mat_ref(0).is_virtual();
}

void block_matrix::materialize_self() const
{
	if (!is_virtual())
		return;

	std::vector<detail::matrix_store::const_ptr> res_stores(store->get_num_mats());
	// TODO materializing individual matrices in serial may hurt performance.
	for (size_t i = 0; i < store->get_num_mats(); i++) {
		dense_matrix::ptr mat = dense_matrix::create(store->get_mat(i));
		mat->materialize_self();
		res_stores[i] = mat->get_raw_store();
	}

	block_matrix *mutable_this = const_cast<block_matrix *>(this);
	mutable_this->store = detail::combined_matrix_store::create(res_stores,
			res_stores[0]->store_layout());
	mutable_this->dense_matrix::assign(*this);
}

void block_matrix::set_materialize_level(materialize_level level,
		detail::matrix_store::ptr materialize_buf)
{
	for (size_t i = 0; i < store->get_num_mats(); i++) {
		detail::matrix_store::const_ptr mat = store->get_mat(i);
		if (mat->is_virtual()) {
			const detail::virtual_matrix_store *tmp
				= dynamic_cast<const detail::virtual_matrix_store *>(mat.get());
			detail::virtual_matrix_store *tmp1
				= const_cast<detail::virtual_matrix_store *>(tmp);
			// TODO we ignore the customization of materialized matrix store.
			tmp1->set_materialize_level(level, NULL);
		}
	}
}

void block_matrix::assign(const dense_matrix &mat)
{
	// The input matrix must be a block matrix. Otherwise, dynamic
	// casting will throw an exception.
	const block_matrix &gmat = dynamic_cast<const block_matrix &>(mat);
	this->store = gmat.store;
	dense_matrix::assign(mat);
}

vector::ptr block_matrix::get_col(off_t idx) const
{
	if ((size_t) idx >= get_num_cols() || idx < 0) {
		BOOST_LOG_TRIVIAL(error) << "the col index is out of bound";
		return vector::ptr();
	}
	if (is_wide()) {
		BOOST_LOG_TRIVIAL(error)
			<< "can't get a column from a group of wide matrices";
		return vector::ptr();
	}

	off_t mat_idx = idx / get_block_size();
	off_t local_idx = idx % get_block_size();
	return vector::create(store->get_mat_ref(mat_idx).get_col_vec(local_idx));
}

vector::ptr block_matrix::get_row(off_t idx) const
{
	if ((size_t) idx >= get_num_rows() || idx < 0) {
		BOOST_LOG_TRIVIAL(error) << "the row index is out of bound";
		return vector::ptr();
	}
	if (!is_wide()) {
		BOOST_LOG_TRIVIAL(error)
			<< "can't get a row from a group of tall matrices";
		return vector::ptr();
	}

	off_t mat_idx = idx / get_block_size();
	off_t local_idx = idx % get_block_size();
	return vector::create(store->get_mat_ref(mat_idx).get_row_vec(local_idx));
}

static void get_local_idxs(const std::vector<off_t> &idxs, size_t block_size,
		std::vector<off_t> &mat_idxs, std::vector<std::vector<off_t> > &local_idxs)
{
	local_idxs.resize(1);
	local_idxs[0].push_back(idxs[0] % block_size);
	mat_idxs.push_back(idxs[0] / block_size);
	for (size_t i = 1; i < idxs.size(); i++) {
		off_t mat_idx = idxs[i] / block_size;
		off_t local_idx = idxs[i] % block_size;
		// We get to a new block.
		if (mat_idx != mat_idxs.back()) {
			mat_idxs.push_back(mat_idx);
			local_idxs.push_back(std::vector<off_t>(1, local_idx));
		}
		else
			local_idxs.back().push_back(local_idx);
	}
	assert(mat_idxs.size() == local_idxs.size());
}

dense_matrix::ptr block_matrix::get_cols(const std::vector<off_t> &idxs) const
{
	if (is_wide()) {
		BOOST_LOG_TRIVIAL(error)
			<< "can't get columns from a group of wide matrices";
		return dense_matrix::ptr();
	}

	for (size_t i = 0; i < idxs.size(); i++) {
		off_t idx = idxs[i];
		if ((size_t) idx >= get_num_cols() || idx < 0) {
			BOOST_LOG_TRIVIAL(error) << "the col index is out of bound";
			return dense_matrix::ptr();
		}
	}

	if (!std::is_sorted(idxs.begin(), idxs.end())) {
		BOOST_LOG_TRIVIAL(error)
			<< "get_cols: the col idxs must be in the ascending order";
		return dense_matrix::ptr();
	}

	std::vector<std::vector<off_t> > local_idxs;
	std::vector<off_t> mat_idxs;
	get_local_idxs(idxs, get_block_size(), mat_idxs, local_idxs);

	std::vector<detail::matrix_store::const_ptr> stores(mat_idxs.size());
	for (size_t i = 0; i < mat_idxs.size(); i++)
		stores[i] = store->get_mat_ref(mat_idxs[i]).get_cols(local_idxs[i]);
	// The block matrix is designed to deal with a group of matrices with
	// the same size.
	return dense_matrix::create(detail::combined_matrix_store::create(stores,
				matrix_layout_t::L_COL));
}

dense_matrix::ptr block_matrix::get_rows(const std::vector<off_t> &idxs) const
{
	if (!is_wide()) {
		BOOST_LOG_TRIVIAL(error)
			<< "can't get rows from a group of tall matrices";
		return dense_matrix::ptr();
	}

	for (size_t i = 0; i < idxs.size(); i++) {
		off_t idx = idxs[i];
		if ((size_t) idx >= get_num_rows() || idx < 0) {
			BOOST_LOG_TRIVIAL(error) << "the row index is out of bound";
			return dense_matrix::ptr();
		}
	}

	if (!std::is_sorted(idxs.begin(), idxs.end())) {
		BOOST_LOG_TRIVIAL(error)
			<< "get_rows: the row idxs must be in the ascending order";
		return dense_matrix::ptr();
	}

	std::vector<std::vector<off_t> > local_idxs;
	std::vector<off_t> mat_idxs;
	get_local_idxs(idxs, get_block_size(), mat_idxs, local_idxs);

	std::vector<detail::matrix_store::const_ptr> stores(mat_idxs.size());
	for (size_t i = 0; i < mat_idxs.size(); i++)
		stores[i] = store->get_mat_ref(mat_idxs[i]).get_rows(local_idxs[i]);
	// The block matrix is designed to deal with a group of matrices with
	// the same size.
	return dense_matrix::create(detail::combined_matrix_store::create(stores,
				matrix_layout_t::L_ROW));
}

dense_matrix::ptr block_matrix::clone() const
{
	return block_matrix::create(store);
}

dense_matrix::ptr block_matrix::transpose() const
{
	detail::matrix_store::const_ptr tmp = store->transpose();
	return block_matrix::create(detail::combined_matrix_store::cast(tmp));
}

static detail::mem_matrix_store::const_ptr get_sub_mat(
		detail::mem_matrix_store::const_ptr mat, size_t start_row,
		size_t start_col, size_t num_rows, size_t num_cols)
{
	detail::local_matrix_store::const_ptr portion = mat->get_portion(
			start_row, start_col, num_rows, num_cols);
	assert(portion);

	detail::mem_matrix_store::ptr ret = detail::mem_matrix_store::create(
			num_rows, num_cols, mat->store_layout(), portion->get_type(), -1);
	ret->write_portion_async(portion, 0, 0);
	return ret;
}

namespace
{

/*
 * This is a summation with generalized operator.
 */
class gsum_op: public fm::detail::portion_mapply_op
{
	bulk_operate::const_ptr op;
public:
	gsum_op(bulk_operate::const_ptr op, size_t out_num_rows,
			size_t out_num_cols): fm::detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->op = op;
	}

	virtual void run(
			const std::vector<fm::detail::local_matrix_store::const_ptr> &ins,
			fm::detail::local_matrix_store &out) const {
		assert(!ins.empty());
		if (ins.size() == 1)
			out.copy_from(*ins[0]);
		else {
			mapply2(*ins[0], *ins[1], *op, out);
			for (size_t i = 2; i < ins.size(); i++)
				mapply2(*ins[i], out, *op, out);
		}
	}

	virtual fm::detail::portion_mapply_op::const_ptr transpose() const;

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		std::string str;
		assert(mats.size() > 1);
		str = "(";
		for (size_t i = 0; i < mats.size() - 1; i++)
			str += mats[i]->get_name() + op->get_name();
		str += mats[mats.size() - 1]->get_name() + ")";
		return str;
	}

	virtual bool is_agg() const {
		return true;
	}

	bulk_operate::const_ptr get_op() const {
		return op;
	}
};

class t_gsum_op: public fm::detail::portion_mapply_op
{
	bulk_operate::const_ptr op;
	gsum_op portion_op;
public:
	t_gsum_op(const gsum_op &_op): fm::detail::portion_mapply_op(
			_op.get_out_num_cols(), _op.get_out_num_rows(),
			_op.get_output_type()), portion_op(_op) {
		this->op = _op.get_op();
	}

	virtual void run(
			const std::vector<fm::detail::local_matrix_store::const_ptr> &ins,
			fm::detail::local_matrix_store &out) const {
		assert(!ins.empty());
		if (ins.size() == 1)
			out.copy_from(*ins[0]);
		else {
			mapply2(*ins[0], *ins[1], *op, out);
			for (size_t i = 2; i < ins.size(); i++)
				mapply2(*ins[i], out, *op, out);
		}
	}

	virtual fm::detail::portion_mapply_op::const_ptr transpose() const {
		return fm::detail::portion_mapply_op::const_ptr(new gsum_op(portion_op));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		return portion_op.to_string(mats);
	}

	virtual bool is_agg() const {
		return true;
	}
};

fm::detail::portion_mapply_op::const_ptr gsum_op::transpose() const
{
	return fm::detail::portion_mapply_op::const_ptr(new t_gsum_op(*this));
}

}

dense_matrix::ptr block_matrix::inner_prod_tall(const dense_matrix &m,
			bulk_operate::const_ptr left_op, bulk_operate::const_ptr right_op,
			matrix_layout_t out_layout) const
{
	// Get the right matrix in memory.
	dense_matrix::ptr m2 = m.conv_store(true, -1);
	detail::mem_matrix_store::const_ptr mem_m2
		= detail::mem_matrix_store::cast(m2->get_raw_store());

	// Here is to reuse the code for matrix multiplication with BLAS.
	bool use_blas = left_op == NULL;
	if (use_blas) {
		assert(get_type() == get_scalar_type<double>()
				|| get_type() == get_scalar_type<float>());
		assert(m.get_type() == get_scalar_type<double>()
				|| m.get_type() == get_scalar_type<float>());
		right_op = bulk_operate::conv2ptr(get_type().get_basic_ops().get_add());
	}

	// This contains the blocks for the final output.
	std::vector<detail::matrix_store::const_ptr> res_blocks(
			div_ceil<size_t>(mem_m2->get_num_cols(), get_block_size()));
	for (size_t m2_col = 0; m2_col < mem_m2->get_num_cols();
			m2_col += get_block_size()) {
		// We multiply with individual matrices and output a vector of
		// temporary matrices. Later on, we need to sum the temporary matrices.
		std::vector<dense_matrix::const_ptr> tmp_mats(store->get_num_mats());
		for (size_t m2_row = 0; m2_row < mem_m2->get_num_rows();
				m2_row += get_block_size()) {
			size_t i = m2_row / get_block_size();
			dense_matrix::ptr left = dense_matrix::create(store->get_mat(i));
			// Get the submatrix in the right matrix
			size_t part_num_rows = std::min(get_block_size(),
					m2->get_num_rows() - m2_row);
			size_t part_num_cols = std::min(get_block_size(),
					m2->get_num_cols() - m2_col);
			detail::mem_matrix_store::const_ptr part = get_sub_mat(mem_m2,
					m2_row, m2_col, part_num_rows, part_num_cols);
			dense_matrix::ptr right = dense_matrix::create(part);

			// Compute the temporary matrix.
			// TODO maybe we can perform multiply with multiple block matrices
			// together in the cost of more memory consumption.
			if (use_blas)
				tmp_mats[i] = left->multiply(*right, out_layout);
			else
				tmp_mats[i] = left->inner_prod(*right, left_op, right_op, out_layout);
			// We really don't need to cache the portion in this intermediate
			// matrix and the EM matrix beneath it in the hierarchy.
			const_cast<detail::matrix_store &>(tmp_mats[i]->get_data()).set_cache_portion(false);
		}

		// We then sum all of the temp matrices.
		detail::portion_mapply_op::const_ptr op(new gsum_op(right_op,
					tmp_mats[0]->get_num_rows(), tmp_mats[0]->get_num_cols()));
		// We will materialize the mapply in a hierarchical way,
		// so the intermediate matrices should be read from SSDs in serial.
		size_t i = m2_col / get_block_size();
		dense_matrix::ptr res = mapply_portion(tmp_mats, op,
				matrix_layout_t::L_COL, false);
		res->materialize_self();
		res_blocks[i] = res->get_raw_store();
	}

	// TODO we need to restore the original caching policy in the EM matrix.

	if (res_blocks.size() == 1)
		return dense_matrix::create(res_blocks[0]);
	else
		return block_matrix::create(detail::combined_matrix_store::create(
					res_blocks, store->store_layout()));
}

dense_matrix::ptr block_matrix::inner_prod_wide(const dense_matrix &m,
			bulk_operate::const_ptr left_op, bulk_operate::const_ptr right_op,
			matrix_layout_t out_layout) const
{
	std::vector<detail::matrix_store::const_ptr> right_mats;
	const block_matrix *block_m = dynamic_cast<const block_matrix *>(&m);
	if (block_m == NULL)
		right_mats.push_back(m.get_raw_store());
	else {
		for (size_t i = 0; i < block_m->store->get_num_mats(); i++)
			right_mats.push_back(block_m->store->get_mat(i));
	}

	if (out_layout == matrix_layout_t::L_NONE) {
		// If the left matrix is in col-major, we prefer the output matrix
		// is also in col-major. It helps computation in local matrices.
		if (store->get_mat_ref(0).store_layout() == matrix_layout_t::L_COL)
			out_layout = matrix_layout_t::L_COL;
		else
			out_layout = matrix_layout_t::L_ROW;
	}

	detail::matrix_store::ptr res = detail::matrix_store::create(
			get_num_rows(), m.get_num_cols(), out_layout,
			right_op->get_output_type(), -1, true);
	// Each time we take one matrix in the right group and perform inner product
	// with all matrices in the left group.
	size_t right_block_size = right_mats[0]->get_num_cols();
	for (size_t i = 0; i < right_mats.size(); i++) {
		dense_matrix::ptr right = dense_matrix::create(right_mats[i]);
		std::vector<dense_matrix::ptr> tmp_mats(store->get_num_mats());
		for (size_t j = 0; j < store->get_num_mats(); j++) {
			dense_matrix::ptr left = dense_matrix::create(store->get_mat(j));
			tmp_mats[j] = left->inner_prod(*right, left_op, right_op,
					out_layout);
			const_cast<detail::matrix_store &>(left->get_data()).set_cache_portion(false);
		}
		materialize(tmp_mats, false);

		// We now copy the inner product result to the final matrix.
		size_t col_idx = i * right_block_size;
		for (size_t j = 0; j < tmp_mats.size(); j++) {
			size_t row_idx = j * block_size;
			size_t num_rows = std::min(block_size, get_num_rows() - row_idx);
			size_t num_cols = std::min(right_block_size,
					m.get_num_cols() - col_idx);
			assert(num_rows == tmp_mats[j]->get_num_rows());
			assert(num_cols == tmp_mats[j]->get_num_cols());
			detail::local_matrix_store::ptr res_part = res->get_portion(row_idx,
					col_idx, num_rows, num_cols);
			detail::local_matrix_store::const_ptr src_part
				= tmp_mats[j]->get_data().get_portion(0);
			res_part->copy_from(*src_part);
		}
	}
	return dense_matrix::create(res);
}

dense_matrix::ptr block_matrix::multiply_tall(const dense_matrix &m,
		matrix_layout_t out_layout) const
{
	return inner_prod_tall(m, NULL, NULL, out_layout);
}

dense_matrix::ptr block_matrix::multiply_wide(const dense_matrix &m,
		matrix_layout_t out_layout) const
{
	return dense_matrix::ptr();
}

dense_matrix::ptr block_matrix::multiply(const dense_matrix &mat,
		matrix_layout_t out_layout) const
{
	if ((get_type() == get_scalar_type<double>()
				|| get_type() == get_scalar_type<float>())) {
		assert(get_type() == mat.get_type());
		size_t long_dim1 = std::max(get_num_rows(), get_num_cols());
		size_t long_dim2 = std::max(mat.get_num_rows(), mat.get_num_cols());
		// We prefer to perform computation on the larger matrix.
		// If the matrix in the right operand is larger, we transpose
		// the entire computation.
		if (long_dim2 > long_dim1) {
			dense_matrix::ptr t_mat1 = this->transpose();
			dense_matrix::ptr t_mat2 = mat.transpose();
			matrix_layout_t t_layout = out_layout;
			if (t_layout == matrix_layout_t::L_ROW)
				t_layout = matrix_layout_t::L_COL;
			else if (t_layout == matrix_layout_t::L_COL)
				t_layout = matrix_layout_t::L_ROW;
			dense_matrix::ptr t_res = t_mat2->multiply(*t_mat1, t_layout);
			return t_res->transpose();
		}

		if (is_wide())
			return multiply_wide(mat, out_layout);
		else
			return multiply_tall(mat, out_layout);
	}
	else
		// This relies on inner product to compute matrix multiplication.
		return dense_matrix::multiply(mat, out_layout);
}

dense_matrix::ptr block_matrix::mapply_cols(vector::const_ptr vals,
		bulk_operate::const_ptr op) const
{
	dense_matrix::ptr tmat = transpose();
	return tmat->mapply_rows(vals, op)->transpose();
}

dense_matrix::ptr block_matrix::mapply_rows(vector::const_ptr vals,
		bulk_operate::const_ptr op) const
{
	if (!vals->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error) << "Can't scale rows with an EM vector";
		return dense_matrix::ptr();
	}
	if (get_num_cols() != vals->get_length()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The vector's length needs to equal to #columns";
		return dense_matrix::ptr();
	}
	
	std::vector<detail::matrix_store::const_ptr> res_stores(
			store->get_num_mats());
	if (is_wide()) {
		for (size_t i = 0; i < res_stores.size(); i++) {
			dense_matrix::ptr mat = dense_matrix::create(store->get_mat(i));
			dense_matrix::ptr res = mat->mapply_rows(vals, op);
			if (res == NULL)
				return dense_matrix::ptr();
			res_stores[i] = res->get_raw_store();
		}
		return block_matrix::create(detail::combined_matrix_store::create(
					res_stores, store->store_layout()));
	}
	else {
		size_t val_start = 0;
		detail::mem_vec_store::const_ptr mem_vals
			= detail::mem_vec_store::cast(vals->get_raw_store());
		for (size_t i = 0; i < res_stores.size(); i++) {
			// Get part of the vector.
			size_t llen = store->get_mat_ref(i).get_num_cols();
			detail::smp_vec_store::ptr vals_store
				= detail::smp_vec_store::create(llen, vals->get_type());
			memcpy(vals_store->get_raw_arr(), mem_vals->get_sub_arr(val_start,
						val_start + llen), llen * vals->get_entry_size());
			vector::ptr vals_part = vector::create(vals_store);

			// Perform computation.
			dense_matrix::ptr mat = dense_matrix::create(store->get_mat(i));
			dense_matrix::ptr res = mat->mapply_rows(vals_part, op);
			if (res == NULL)
				return dense_matrix::ptr();
			res_stores[i] = res->get_raw_store();

			val_start += llen;
		}
		assert(val_start == vals->get_length());
		return block_matrix::create(detail::combined_matrix_store::create(
					res_stores, store->store_layout()));
	}
}

dense_matrix::ptr block_matrix::mapply2(const dense_matrix &m,
		bulk_operate::const_ptr op) const
{
	if (get_num_rows() != m.get_num_rows()
			|| get_num_cols() != m.get_num_cols()) {
		BOOST_LOG_TRIVIAL(error) << "The matrix size isn't compatible";
		return dense_matrix::ptr();
	}
	const block_matrix *block_m = dynamic_cast<const block_matrix *>(&m);
	if (block_m == NULL) {
		BOOST_LOG_TRIVIAL(error) << "The input matrix isn't a block matrix";
		return dense_matrix::ptr();
	}
	if (block_m->get_block_size() != get_block_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The input matrix has a different block size";
		return dense_matrix::ptr();
	}

	std::vector<detail::matrix_store::const_ptr> res_stores(
			store->get_num_mats());
	for (size_t i = 0; i < res_stores.size(); i++) {
		dense_matrix::ptr mat1 = dense_matrix::create(store->get_mat(i));
		dense_matrix::ptr mat2 = dense_matrix::create(block_m->store->get_mat(i));
		dense_matrix::ptr lres = mat1->mapply2(*mat2, op);
		if (lres == NULL)
			return dense_matrix::ptr();

		res_stores[i] = lres->get_raw_store();
	}
	return block_matrix::create(detail::combined_matrix_store::create(
				res_stores, store->store_layout()));
}

dense_matrix::ptr block_matrix::sapply(bulk_uoperate::const_ptr op) const
{
	std::vector<detail::matrix_store::const_ptr> res_stores(
			store->get_num_mats());
	for (size_t i = 0; i < res_stores.size(); i++) {
		dense_matrix::ptr mat = dense_matrix::create(store->get_mat(i));
		mat = mat->sapply(op);
		if (mat == NULL)
			return dense_matrix::ptr();

		res_stores[i] = mat->get_raw_store();
	}
	return block_matrix::create(detail::combined_matrix_store::create(
				res_stores, store->store_layout()));
}

dense_matrix::ptr block_matrix::apply(matrix_margin margin,
		arr_apply_operate::const_ptr op) const
{
	// TODO
	assert(0);
	return dense_matrix::ptr();
}

}
