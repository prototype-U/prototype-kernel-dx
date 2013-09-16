#ifndef __SHMEM_FS_H
#define __SHMEM_FS_H

#include <linux/swap.h>
#include <linux/mempolicy.h>
#include <linux/pagemap.h>
#include <linux/percpu_counter.h>


struct shmem_inode_info {
	spinlock_t		lock;
	unsigned long		flags;
	unsigned long		alloced;	
	union {
		unsigned long	swapped;	
		char		*symlink;	
	};
	struct shared_policy	policy;		
	struct list_head	swaplist;	
	struct list_head	xattr_list;	
	struct inode		vfs_inode;
};

struct shmem_sb_info {
	unsigned long max_blocks;   
	struct percpu_counter used_blocks;  
	unsigned long max_inodes;   
	unsigned long free_inodes;  
	spinlock_t stat_lock;	    
	uid_t uid;		    
	gid_t gid;		    
	umode_t mode;		    
	struct mempolicy *mpol;     
};

static inline struct shmem_inode_info *SHMEM_I(struct inode *inode)
{
	return container_of(inode, struct shmem_inode_info, vfs_inode);
}

extern int shmem_init(void);
extern int shmem_fill_super(struct super_block *sb, void *data, int silent);
extern struct file *shmem_file_setup(const char *name,
					loff_t size, unsigned long flags);
extern int shmem_zero_setup(struct vm_area_struct *);
extern int shmem_lock(struct file *file, int lock, struct user_struct *user);
extern void shmem_unlock_mapping(struct address_space *mapping);
extern struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
					pgoff_t index, gfp_t gfp_mask);
extern void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end);
extern int shmem_unuse(swp_entry_t entry, struct page *page);

static inline struct page *shmem_read_mapping_page(
				struct address_space *mapping, pgoff_t index)
{
	return shmem_read_mapping_page_gfp(mapping, index,
					mapping_gfp_mask(mapping));
}

#endif
