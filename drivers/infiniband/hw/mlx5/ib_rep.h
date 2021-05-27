/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2018 Mellanox Technologies. All rights reserved.
 */

#ifndef __MLX5_IB_REP_H__
#define __MLX5_IB_REP_H__

#include <linux/mlx5/eswitch.h>
#include "mlx5_ib.h"

extern const struct mlx5_ib_profile raw_eth_profile;

#ifdef CONFIG_MLX5_ESWITCH
u8 mlx5_ib_eswitch_mode(struct mlx5_eswitch *esw);
struct mlx5_ib_dev *mlx5_ib_get_rep_ibdev(struct mlx5_eswitch *esw,
					  u16 vport_num);
struct mlx5_ib_dev *mlx5_ib_get_uplink_ibdev(struct mlx5_eswitch *esw);
struct mlx5_eswitch_rep *mlx5_ib_vport_rep(struct mlx5_eswitch *esw,
					   u16 vport_num);
void mlx5_ib_register_vport_reps(struct mlx5_core_dev *mdev);
void mlx5_ib_unregister_vport_reps(struct mlx5_core_dev *mdev);
struct mlx5_flow_handle *create_flow_rule_vport_sq(struct mlx5_ib_dev *dev,
						   struct mlx5_ib_sq *sq,
						   u32 port);
struct net_device *mlx5_ib_get_rep_netdev(struct mlx5_eswitch *esw,
					  u16 vport_num);
u32 mlx5_ib_eswitch_vport_match_metadata_enabled(struct mlx5_eswitch *esw);
u32 mlx5_ib_eswitch_get_vport_metadata_for_match(struct mlx5_eswitch *esw,
		                                 u16 vport);
u32 mlx5_ib_eswitch_get_vport_metadata_mask(void);
#else /* CONFIG_MLX5_ESWITCH */
static inline u8 mlx5_ib_eswitch_mode(struct mlx5_eswitch *esw)
{
	return MLX5_ESWITCH_NONE;
}

static inline
struct mlx5_ib_dev *mlx5_ib_get_rep_ibdev(struct mlx5_eswitch *esw,
					  u16 vport_num)
{
	return NULL;
}

static inline
struct mlx5_ib_dev *mlx5_ib_get_uplink_ibdev(struct mlx5_eswitch *esw)
{
	return NULL;
}

static inline
struct mlx5_eswitch_rep *mlx5_ib_vport_rep(struct mlx5_eswitch *esw,
					   u16 vport_num)
{
	return NULL;
}

static inline void mlx5_ib_register_vport_reps(struct mlx5_core_dev *mdev) {}
static inline void mlx5_ib_unregister_vport_reps(struct mlx5_core_dev *mdev) {}
static inline
struct mlx5_flow_handle *create_flow_rule_vport_sq(struct mlx5_ib_dev *dev,
						   struct mlx5_ib_sq *sq,
						   u32 port)
{
	return NULL;
}

static inline
struct net_device *mlx5_ib_get_rep_netdev(struct mlx5_eswitch *esw,
					  u16 vport_num)
{
	return NULL;
}

static inline
u32 mlx5_ib_eswitch_vport_match_metadata_enabled(struct mlx5_eswitch *esw)
{
	return 0;
};

static inline
u32 mlx5_ib_eswitch_get_vport_metadata_for_match(struct mlx5_eswitch *esw,
				        	 u16 vport)
{
	return 0;
};

static inline
u32 mlx5_ib_eswitch_get_vport_metadata_mask(void)
{
	return 0;
};
#endif

static inline
struct mlx5_ib_dev *mlx5_ib_rep_to_dev(struct mlx5_eswitch_rep *rep)
{
	return rep->rep_data[REP_IB].priv;
}
#endif /* __MLX5_IB_REP_H__ */
