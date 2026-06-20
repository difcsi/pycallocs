#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bintree
{
    int val;
    struct bintree *left;
    struct bintree *right;
};

void print_bintree(const struct bintree *root)
{
    printf("{");
    if (root->left) print_bintree(root->left);
    printf("%d", root->val);
    if (root->right) print_bintree(root->right);
    printf("}");
}

void bst_insert(struct bintree *bst, int value)
{
    if (value < bst->val)
    {
        if (bst->left) bst_insert(bst->left, value);
        else
        {
            bst->left = malloc(sizeof(struct bintree));
            *bst->left = (struct bintree){.val = value};
        }
    }
    else
    {
        if (bst->right) bst_insert(bst->right, value);
        else
        {
            bst->right = malloc(sizeof(struct bintree));
            *bst->right = (struct bintree){.val = value};
        }
    }
}

void free_bintree(struct bintree *root)
{
    if (!root) return;
    free_bintree(root->left);
    free_bintree(root->right);
    free(root);
}

struct bintree *bst_copy_node(struct bintree *node)
{
    struct bintree *new_bt = malloc(sizeof(struct bintree));
    memcpy(new_bt, node, sizeof(struct bintree));
    return new_bt;
}
