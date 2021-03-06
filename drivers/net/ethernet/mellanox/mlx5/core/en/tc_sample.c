// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2020 Mellanox Technologies. */

#include "en/tc_sample.h"
#include "eswitch.h"
#include "en_tc.h"
#include "fs_core.h"
#include "en/mapping.h"

#define MLX5_ESW_VPORT_TBL_SIZE_SAMPLE (64 * 1024)

static const struct esw_vport_tbl_namespace mlx5_esw_vport_tbl_sample_ns = {
	.max_fte = MLX5_ESW_VPORT_TBL_SIZE_SAMPLE,
	.max_num_groups = 0,    /* default num of groups */
	.flags = MLX5_FLOW_TABLE_TUNNEL_EN_REFORMAT | MLX5_FLOW_TABLE_TUNNEL_EN_DECAP,
};

struct mlx5_tc_psample {
	struct mlx5e_priv *priv;
	struct idr *fte_ids;

	struct mlx5_flow_table *termtbl;
	struct mlx5_flow_handle *termtbl_rule;

	DECLARE_HASHTABLE(sampler_hashtbl, 8);
	struct mutex sampler_mutex; /* protect sampler_hashtbl */

	DECLARE_HASHTABLE(sample_restore_hashtbl, 8);
	struct mutex sample_restore_mutex; /* protect sample_restore_hashtbl */
};

struct mlx5_sampler {
	struct hlist_node sampler_hlist;

	u32 sampler_id;
	u32 sample_ratio;
	u32 sample_table_id;
	u32 default_table_id;
	int ref_count;
};

struct mlx5_sample_flow {
	struct mlx5_sampler *sampler;
	struct mlx5_sample_restore *restore_context;
	struct mlx5_flow_attr *pre_attr;
	struct mlx5_flow_handle *pre_rule;
	struct mlx5_flow_handle *rule;
	u32 fte_id;
};

struct mlx5_sample_restore {
	struct hlist_node restore_hlist;

	struct mlx5_modify_hdr *modify_hdr;
	struct mlx5_flow_handle *restore_rule;
	u32 obj_id;
	int ref_count;
};

static int
mlx5_tc_sample_termtbl_create(struct mlx5_tc_psample *tc_psample)
{
	struct mlx5_core_dev *dev = tc_psample->priv->mdev;
	struct mlx5_flow_table_attr ft_attr = {};
	struct mlx5_flow_destination dest = {};
	struct mlx5_flow_namespace *root_ns;
	struct mlx5_flow_act act = {};
	int err;

	if (!MLX5_CAP_ESW_FLOWTABLE_FDB(dev, termination_table))  {
		mlx5_core_warn(dev, "%s: termination table is not supported\n", __func__);
		return -EOPNOTSUPP;
	}

	root_ns = mlx5_get_flow_namespace(dev, MLX5_FLOW_NAMESPACE_FDB);
	if (!root_ns) {
		mlx5_core_warn(dev, "%s: failed to get FDB flow namespace\n", __func__);
		return -EOPNOTSUPP;
	}

	ft_attr.flags = MLX5_FLOW_TABLE_TERMINATION | MLX5_FLOW_TABLE_UNMANAGED;
	ft_attr.autogroup.max_num_groups = 1;
	ft_attr.prio = FDB_SLOW_PATH;
	ft_attr.max_fte = 1;
	ft_attr.level = 1;
	tc_psample->termtbl = mlx5_create_auto_grouped_flow_table(root_ns, &ft_attr);
	if (IS_ERR(tc_psample->termtbl)) {
		err = PTR_ERR(tc_psample->termtbl);
		mlx5_core_warn(dev, "%s: failed to create termtbl, err: %d\n", __func__, err);
		return err;
	}

	act.action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	dest.type = MLX5_FLOW_DESTINATION_TYPE_VPORT;
	if (mlx5_core_is_ecpf_esw_manager(dev))
		dest.vport.num = MLX5_VPORT_ECPF;
	else
		dest.vport.num = MLX5_VPORT_PF;
	tc_psample->termtbl_rule = mlx5_add_flow_rules(tc_psample->termtbl, NULL, &act, &dest, 1);
	if (IS_ERR(tc_psample->termtbl_rule)) {
		err = PTR_ERR(tc_psample->termtbl_rule);
		mlx5_core_warn(dev, "%s: failed to create termtbl rule, err: %d\n", __func__, err);
		mlx5_destroy_flow_table(tc_psample->termtbl);
		return err;
	}

	return 0;
}

static void
mlx5_tc_sample_termtbl_destroy(struct mlx5_tc_psample *tc_psample)
{
	mlx5_del_flow_rules(tc_psample->termtbl_rule);
	mlx5_destroy_flow_table(tc_psample->termtbl);
}

static int
mlx5_tc_sample_create_sampler_obj(struct mlx5_core_dev *mdev,
				  struct mlx5_sampler *sampler,
				  int *sampler_id)
{
	u32 in[MLX5_ST_SZ_DW(create_sampler_obj_in)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];
	u64 general_obj_types;
	void *obj;
	int err;

	general_obj_types = MLX5_CAP_GEN_64(mdev, general_obj_types);
	if (!(general_obj_types & MLX5_HCA_CAP_GENERAL_OBJECT_TYPES_SAMPLER))
		return -EOPNOTSUPP;

	obj = MLX5_ADDR_OF(create_sampler_obj_in, in, sampler_object);
	MLX5_SET(sampler_obj, obj, table_type, FS_FT_FDB);
	MLX5_SET(sampler_obj, obj, ignore_flow_level, 1);
	MLX5_SET(sampler_obj, obj, level, 1);
	MLX5_SET(sampler_obj, obj, sample_ratio, sampler->sample_ratio);
	MLX5_SET(sampler_obj, obj, sample_table_id, sampler->sample_table_id);
	MLX5_SET(sampler_obj, obj, default_table_id, sampler->default_table_id);
	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_GENERAL_OBJECT_TYPES_SAMPLER);

	err = mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
	if (!err)
		*sampler_id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return err;
}

static void
mlx5_tc_sample_destroy_sampler_obj(struct mlx5_core_dev *mdev, u32 sampler_id)
{
	u32 in[MLX5_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_DESTROY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_GENERAL_OBJECT_TYPES_SAMPLER);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, sampler_id);

	mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
}

static u32
mlx5_tc_sample_sampler_hash(u32 sample_ratio, u32 default_table_id)
{
	return jhash_2words(sample_ratio, default_table_id, 0);
}

static int
mlx5_tc_sample_sampler_cmp(u32 sample_ratio1, u32 default_table_id1,
			   u32 sample_ratio2, u32 default_table_id2)
{
	return sample_ratio1 != sample_ratio2 || default_table_id1 != default_table_id2;
}

static struct mlx5_sampler *
mlx5_tc_sample_sampler_get_create(struct mlx5_tc_psample *tc_psample, u32 sample_ratio,
				  u32 default_table_id)
{
	struct mlx5_sampler *sampler;
	u32 hash_key;
	int err;

	mutex_lock(&tc_psample->sampler_mutex);
	hash_key = mlx5_tc_sample_sampler_hash(sample_ratio, default_table_id);
	hash_for_each_possible(tc_psample->sampler_hashtbl, sampler, sampler_hlist, hash_key)
		if (!mlx5_tc_sample_sampler_cmp(sampler->sample_ratio, sampler->default_table_id,
						sample_ratio, default_table_id))
			goto add_ref;

	sampler = kzalloc(sizeof(*sampler), GFP_KERNEL);
	if (!sampler) {
		err = -ENOMEM;
		goto err_alloc;
	}

	sampler->sample_table_id = tc_psample->termtbl->id;
	sampler->default_table_id = default_table_id;
	sampler->sample_ratio = sample_ratio;

	err = mlx5_tc_sample_create_sampler_obj(tc_psample->priv->mdev, sampler,
						&sampler->sampler_id);
	if (err)
		goto err_create;

	hash_add(tc_psample->sampler_hashtbl, &sampler->sampler_hlist, hash_key);

add_ref:
	sampler->ref_count++;
	mutex_unlock(&tc_psample->sampler_mutex);
	return sampler;

err_create:
	kfree(sampler);
err_alloc:
	mutex_unlock(&tc_psample->sampler_mutex);
	return ERR_PTR(err);
}

static void
mlx5_tc_sample_sampler_put(struct mlx5_tc_psample *tc_psample, struct mlx5_sampler *sampler)
{
	mutex_lock(&tc_psample->sampler_mutex);
	if (--sampler->ref_count == 0) {
		hash_del(&sampler->sampler_hlist);
		mlx5_tc_sample_destroy_sampler_obj(tc_psample->priv->mdev, sampler->sampler_id);
		kfree(sampler);
	}
	mutex_unlock(&tc_psample->sampler_mutex);
}

static struct mlx5_modify_hdr *
mlx5_tc_sample_metadata_rule(struct mlx5_core_dev *mdev, u32 obj_id, int tunnel_id, u32 fte_id)
{
	struct mlx5e_tc_mod_hdr_acts mod_acts = {};
	struct mlx5_modify_hdr *modify_hdr;
	int err;

	err = mlx5e_tc_match_to_reg_set(mdev, &mod_acts, MLX5_FLOW_NAMESPACE_FDB,
					CHAIN_TO_REG, obj_id);
	if (err)
		goto err_set_regc0;

	if (tunnel_id) {
		err = mlx5e_tc_match_to_reg_set(mdev, &mod_acts, MLX5_FLOW_NAMESPACE_FDB,
						TUNNEL_TO_REG, tunnel_id);
		if (err)
			goto err_set_regc1;

		err = mlx5e_tc_match_to_reg_set(mdev, &mod_acts, MLX5_FLOW_NAMESPACE_FDB,
						FTEID_TO_REG, fte_id);
		if (err)
			goto err_set_regc5;
	}

	modify_hdr = mlx5_modify_header_alloc(mdev, MLX5_FLOW_NAMESPACE_FDB,
					      mod_acts.num_actions,
					      mod_acts.actions);
	if (IS_ERR(modify_hdr)) {
		err = PTR_ERR(modify_hdr);
		goto err_modify_hdr;
	}

	dealloc_mod_hdr_actions(&mod_acts);
	return modify_hdr;

err_modify_hdr:
err_set_regc5:
err_set_regc1:
	dealloc_mod_hdr_actions(&mod_acts);
err_set_regc0:
	return ERR_PTR(err);
}

static struct mlx5_sample_restore *
mlx5_tc_sample_restore_get_create(struct mlx5_tc_psample *tc_psample, u32 obj_id, int tunnel_id,
				  u32 fte_id)
{
	struct mlx5_core_dev *mdev = tc_psample->priv->mdev;
	struct mlx5_eswitch *esw = mdev->priv.eswitch;
	struct mlx5_sample_restore *restore_context;
	struct mlx5_modify_hdr *modify_hdr;
	int err;

	mutex_lock(&tc_psample->sample_restore_mutex);
	hash_for_each_possible(tc_psample->sample_restore_hashtbl, restore_context,
			       restore_hlist, obj_id)
		if (restore_context->obj_id == obj_id)
			goto add_ref;

	restore_context = kzalloc(sizeof(*restore_context), GFP_KERNEL);
	if (!restore_context) {
		err = -ENOMEM;
		goto err_alloc;
	}
	restore_context->obj_id = obj_id;

	modify_hdr = mlx5_tc_sample_metadata_rule(mdev, obj_id, tunnel_id, fte_id);
	if (IS_ERR(modify_hdr)) {
		err = PTR_ERR(modify_hdr);
		goto err_modify_hdr;
	}
	restore_context->modify_hdr = modify_hdr;

	restore_context->restore_rule = esw_add_restore_rule(esw, obj_id);
	if (IS_ERR(restore_context->restore_rule)) {
		err = PTR_ERR(restore_context->restore_rule);
		goto err_restore;
	}

	hash_add(tc_psample->sample_restore_hashtbl, &restore_context->restore_hlist, obj_id);
add_ref:
	restore_context->ref_count++;
	mutex_unlock(&tc_psample->sample_restore_mutex);
	return restore_context;

err_restore:
	mlx5_modify_header_dealloc(mdev, restore_context->modify_hdr);
err_modify_hdr:
	kfree(restore_context);
err_alloc:
	mutex_unlock(&tc_psample->sample_restore_mutex);
	return ERR_PTR(err);
}

static void
mlx5_tc_sample_restore_put(struct mlx5_tc_psample *tc_psample,
			   struct mlx5_sample_restore *restore_context)
{
	mutex_lock(&tc_psample->sample_restore_mutex);
	if (--restore_context->ref_count == 0)
		hash_del(&restore_context->restore_hlist);
	mutex_unlock(&tc_psample->sample_restore_mutex);

	if (!restore_context->ref_count) {
		mlx5_del_flow_rules(restore_context->restore_rule);
		mlx5_modify_header_dealloc(tc_psample->priv->mdev, restore_context->modify_hdr);
		kfree(restore_context);
	}
}

/* For the following typical flow table:
 *
 * +-------------------------------+
 * +       original flow table     +
 * +-------------------------------+
 * +         original match        +
 * +-------------------------------+
 * + sample action + other actions +
 * +-------------------------------+
 *
 * We translate the tc filter with sample action to the following HW model:
 *
 *         +---------------------+
 *         + original flow table +
 *         +---------------------+
 *         +   original match    +
 *         +---------------------+
 *                    |
 *                    v
 * +------------------------------------------------+
 * +                Flow Sampler Object             +
 * +------------------------------------------------+
 * +                    sample ratio                +
 * +------------------------------------------------+
 * +    sample table id    |    default table id    +
 * +------------------------------------------------+
 *            |                            |
 *            v                            v
 * +-----------------------------+  +----------------------------------------+
 * +        sample table         +  + default table per <vport, chain, prio> +
 * +-----------------------------+  +----------------------------------------+
 * + forward to management vport +  +            original match              +
 * +-----------------------------+  +----------------------------------------+
 *                                  +            other actions               +
 *                                  +----------------------------------------+
 *
 * If there is decap action, do it in the original flow table.
 *
 * +--------------------------------+
 * +       original flow table      +
 * +--------------------------------+
 * +         original match         +
 * +--------------------------------+
 * + sample + decap + other actions +
 * +--------------------------------+
 *
 * We translate the tc filter with sample and decap action to the following HW model:
 *
 *         +---------------------+
 *         + original flow table +
 *         +---------------------+
 *         +   original match    +
 *         +---------------------+
 *         +       decap         +
 *         +---------------------+
 *               | set fte_id
 *               v
 * +------------------------------------------------+
 * +                Flow Sampler Object             +
 * +------------------------------------------------+
 * +                    sample ratio                +
 * +------------------------------------------------+
 * +    sample table id    |    default table id    +
 * +------------------------------------------------+
 *            |                            |
 *            v                            v
 * +-----------------------------+  +----------------------------------------+
 * +        sample table         +  + default table per <vport, chain, prio> +
 * +-----------------------------+  +----------------------------------------+
 * + forward to management vport +  +            fte_id match                +
 * +-----------------------------+  +----------------------------------------+
 *                                  +            other actions               +
 *                                  +----------------------------------------+
 */
struct mlx5_flow_handle *
mlx5_tc_sample_offload(struct mlx5_tc_psample *tc_psample,
		       struct mlx5_flow_spec *spec,
		       struct mlx5_flow_attr *attr,
		       int tunnel_id)
{
	struct mlx5_esw_flow_attr *esw_attr = attr->esw_attr;
	struct mlx5_vport_tbl_attr per_vport_tbl_attr;
	struct mlx5_esw_flow_attr *pre_esw_attr;
	struct mlx5_mapped_obj restore_obj = {};
	struct mlx5_sample_flow *sample_flow;
	struct mlx5_sample_attr *sample_attr;
	struct mlx5_flow_table *default_tbl;
	struct mlx5_flow_spec *fteid_spec = NULL;
	struct mlx5_flow_attr *pre_attr;
	struct mlx5_eswitch *esw;
	u8 outer_match_level;
	u8 inner_match_level;
	bool decap = false;
	u32 fte_id = 1;
	u32 obj_id;
	int err;

	if (IS_ERR_OR_NULL(tc_psample))
		return ERR_PTR(-EOPNOTSUPP);

	sample_flow = kzalloc(sizeof(*sample_flow), GFP_KERNEL);
	if (!sample_flow)
		return ERR_PTR(-ENOMEM);
	esw_attr->sample->sample_flow = sample_flow;

	err = idr_alloc_u32(tc_psample->fte_ids, sample_flow, &fte_id,
			    MLX5_FTE_ID_MAX, GFP_KERNEL);
	if (err) {
		netdev_warn(tc_psample->priv->netdev,
			    "Failed to allocate sample fte id, err: %d\n", err);
		goto err_idr;
	}
	sample_flow->fte_id = fte_id;

	/* Allocate default table per vport, chain and prio. Otherwise, there is
	 * only one default table for the same sampler object. Rules with different
	 * prio and chain may overlap. For CT sample action, per vport default
	 * table is needed to resotre the metadata.
	 */
	per_vport_tbl_attr.chain = attr->chain;
	per_vport_tbl_attr.prio = attr->prio;
	per_vport_tbl_attr.vport = esw_attr->in_rep->vport;
	per_vport_tbl_attr.vport_ns = &mlx5_esw_vport_tbl_sample_ns;
	esw = tc_psample->priv->mdev->priv.eswitch;
	default_tbl = esw_vport_tbl_get(esw, &per_vport_tbl_attr);
	if (IS_ERR(default_tbl)) {
		err = PTR_ERR(default_tbl);
		goto err_default_tbl;
	}

	/* Perform the original matches on the default table.
	 * Offload all actions except the sample action.
	 */
	esw_attr->sample->sample_default_tbl = default_tbl;
	mlx5_eswitch_clear_rule_source_port(esw, spec, esw_attr);
	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_DECAP) {
		decap = true;
		attr->action &= ~MLX5_FLOW_CONTEXT_ACTION_DECAP;

		fteid_spec = kvzalloc(sizeof(*fteid_spec), GFP_KERNEL);
		if (!fteid_spec) {
			err = -ENOMEM;
			goto err_fteid_spec;
		}
		mlx5e_tc_match_to_reg_match(fteid_spec, FTEID_TO_REG, fte_id, MLX5_FTE_ID_MAX);

		inner_match_level = attr->inner_match_level;
		outer_match_level = attr->outer_match_level;
		attr->inner_match_level = MLX5_MATCH_NONE;
		attr->outer_match_level = MLX5_MATCH_NONE;
		sample_flow->rule = mlx5_eswitch_add_offloaded_rule(esw, fteid_spec, attr);
		if (IS_ERR(sample_flow->rule)) {
			err = PTR_ERR(sample_flow->rule);
			goto err_offload_rule;
		}
		attr->inner_match_level = inner_match_level;
		attr->outer_match_level = outer_match_level;
	} else {
		sample_flow->rule = mlx5_eswitch_add_offloaded_rule(esw, spec, attr);
		if (IS_ERR(sample_flow->rule)) {
			err = PTR_ERR(sample_flow->rule);
			goto err_offload_rule;
		}
	}

	/* Create sampler object. */
	sample_flow->sampler = mlx5_tc_sample_sampler_get_create(tc_psample,
								 esw_attr->sample->rate,
								 default_tbl->id);
	if (IS_ERR(sample_flow->sampler)) {
		err = PTR_ERR(sample_flow->sampler);
		goto err_sampler;
	}

	/* Create an id mapping reg_c0 value to sample object. */
	restore_obj.type = MLX5_MAPPED_OBJ_SAMPLE;
	restore_obj.sample.group_id = esw_attr->sample->group_num;
	restore_obj.sample.rate = esw_attr->sample->rate;
	restore_obj.sample.trunc_size = esw_attr->sample->trunc_size;
	err = mapping_add(esw->offloads.reg_c0_obj_pool, &restore_obj, &obj_id);
	if (err)
		goto err_obj_id;
	esw_attr->sample->restore_obj_id = obj_id;

	/* Create sample restore context. */
	sample_flow->restore_context = mlx5_tc_sample_restore_get_create(tc_psample, obj_id,
									 tunnel_id, fte_id);
	if (IS_ERR(sample_flow->restore_context)) {
		err = PTR_ERR(sample_flow->restore_context);
		goto err_sample_restore;
	}

	/* Perform the original matches on the original table. Offload the
	 * sample action. The destination is the sampler object.
	 */
	pre_attr = mlx5_alloc_flow_attr(MLX5_FLOW_NAMESPACE_FDB);
	if (!pre_attr) {
		err = -ENOMEM;
		goto err_alloc_flow_attr;
	}
	sample_attr = kzalloc(sizeof(*sample_attr), GFP_KERNEL);
	if (!sample_attr) {
		err = -ENOMEM;
		goto err_alloc_sample_attr;
	}
	pre_esw_attr = pre_attr->esw_attr;
	pre_attr->action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST | MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
	if (decap)
		pre_attr->action |= MLX5_FLOW_CONTEXT_ACTION_DECAP;
	pre_attr->modify_hdr = sample_flow->restore_context->modify_hdr;
	pre_attr->flags = MLX5_ESW_ATTR_FLAG_SAMPLE;
	pre_attr->inner_match_level = attr->inner_match_level;
	pre_attr->outer_match_level = attr->outer_match_level;
	pre_attr->chain = attr->chain;
	pre_attr->prio = attr->prio;
	pre_esw_attr->sample = sample_attr;
	pre_esw_attr->sample->sampler_id = sample_flow->sampler->sampler_id;
	pre_esw_attr->in_mdev = esw_attr->in_mdev;
	pre_esw_attr->in_rep = esw_attr->in_rep;
	sample_flow->pre_rule = mlx5_eswitch_add_offloaded_rule(esw, spec, pre_attr);
	if (IS_ERR(sample_flow->pre_rule)) {
		err = PTR_ERR(sample_flow->pre_rule);
		goto err_pre_offload_rule;
	}
	sample_flow->pre_attr = pre_attr;

	if (decap)
		kfree(fteid_spec);

	return sample_flow->rule;

err_pre_offload_rule:
	kfree(sample_attr);
err_alloc_sample_attr:
	kfree(pre_attr);
err_alloc_flow_attr:
	mlx5_tc_sample_restore_put(tc_psample, sample_flow->restore_context);
err_sample_restore:
	mapping_remove(esw->offloads.reg_c0_obj_pool, obj_id);
err_obj_id:
	mlx5_tc_sample_sampler_put(tc_psample, sample_flow->sampler);
err_sampler:
	/* For sample offload, rule is added in default_tbl. No need to call
	 * mlx5_esw_chains_put_table()
	 */
	attr->prio = 0;
	attr->chain = 0;
	mlx5_eswitch_del_offloaded_rule(esw, sample_flow->rule, attr);
err_offload_rule:
	if (decap)
		kfree(fteid_spec);
err_fteid_spec:
	esw_vport_tbl_put(esw, &per_vport_tbl_attr);
err_default_tbl:
	idr_remove(tc_psample->fte_ids, fte_id);
err_idr:
	kfree(sample_flow);

	return ERR_PTR(err);
}

void
mlx5_tc_sample_unoffload(struct mlx5_tc_psample *tc_psample,
			 struct mlx5_flow_handle *rule,
			 struct mlx5_flow_attr *attr)
{
	struct mlx5_esw_flow_attr *esw_attr = attr->esw_attr;
	struct mlx5_sample_flow *sample_flow;
	struct mlx5_vport_tbl_attr tbl_attr;
	struct mlx5_flow_attr *pre_attr;
	struct mlx5_eswitch *esw;

	if (IS_ERR_OR_NULL(tc_psample))
		return;

	esw = tc_psample->priv->mdev->priv.eswitch;
	sample_flow = esw_attr->sample->sample_flow;
	pre_attr = sample_flow->pre_attr;
	memset(pre_attr, 0, sizeof(*pre_attr));
	esw = tc_psample->priv->mdev->priv.eswitch;
	mlx5_eswitch_del_offloaded_rule(esw, sample_flow->pre_rule, pre_attr);
	mlx5_eswitch_del_offloaded_rule(esw, sample_flow->rule, attr);

	mlx5_tc_sample_restore_put(tc_psample, sample_flow->restore_context);
	mapping_remove(esw->offloads.reg_c0_obj_pool, esw_attr->sample->restore_obj_id);
	mlx5_tc_sample_sampler_put(tc_psample, sample_flow->sampler);
	tbl_attr.chain = attr->chain;
	tbl_attr.prio = attr->prio;
	tbl_attr.vport = esw_attr->in_rep->vport;
	tbl_attr.vport_ns = &mlx5_esw_vport_tbl_sample_ns;
	esw_vport_tbl_put(esw, &tbl_attr);

	idr_remove(tc_psample->fte_ids, sample_flow->fte_id);
	kfree(pre_attr->esw_attr->sample);
	kfree(pre_attr);
	kfree(sample_flow);
}

struct mlx5_tc_psample *
mlx5_tc_sample_init(struct mlx5e_priv *priv, struct idr *fte_ids)
{
	struct mlx5_tc_psample *tc_psample;
	int err;

	tc_psample = kzalloc(sizeof(*tc_psample), GFP_KERNEL);
	if (!tc_psample) {
		err = -ENOMEM;
		goto err_alloc;
	}
	tc_psample->priv = priv;
	err = mlx5_tc_sample_termtbl_create(tc_psample);
	if (err)
		goto err_termtbl;

	mutex_init(&tc_psample->sampler_mutex);
	mutex_init(&tc_psample->sample_restore_mutex);
	tc_psample->fte_ids = fte_ids;

	return tc_psample;

err_termtbl:
	kfree(tc_psample);
err_alloc:
	return ERR_PTR(err);
}

void
mlx5_tc_sample_clean(struct mlx5_tc_psample *tc_psample)
{
	if (IS_ERR_OR_NULL(tc_psample))
		return;
	mutex_destroy(&tc_psample->sample_restore_mutex);
	mutex_destroy(&tc_psample->sampler_mutex);
	mlx5_tc_sample_termtbl_destroy(tc_psample);
	kfree(tc_psample);
}
