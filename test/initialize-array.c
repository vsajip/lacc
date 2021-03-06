int printf(const char *, ...);

int foo[2][3] = {{1, 2,}, 3, 4, 5};

long bar[3][2] = {{1, 2}, {3, 4,}, {5, 6}};

struct {
	int a[3], b;
} baz[] = {{{1}}, {{2, 7, 4}, 4,}};

struct {
	unsigned char e[4];
} pc = {{31, 2, 3, 4,}};

struct {
	char v[4];
	int k;
} obj = {1, 2, 3,};

void verify(void) {
	int i, j;
	for (i = 0; i < 2; ++i)
		for (j = 0; j < 3; ++j)
			printf("foo[%d][%d] = %d\n", i, j, foo[i][j]);
	for (i = 0; i < 3; ++i)
		for (j = 0; j < 2; ++j)
			printf("bar[%d][%d] = %ld\n", i, j, bar[i][j]);
	for (i = 0; i < 2; ++i)
		printf("baz[%d] = {{%d, %d, %d}, %d}\n",
			i, baz[i].a[0], baz[i].a[1], baz[i].a[2], baz[i].b);
	printf("pc = {{%d, %d, %d, %d}}\n", pc.e[0], pc.e[1], pc.e[2], pc.e[3]);
	printf("obj = {{%d, %d, %d, %d}, %d}, size = %lu\n",
		obj.v[0], obj.v[1], obj.v[2], obj.v[3], obj.k, sizeof(obj));
}

int main(void) {
	verify();
	return 0;
}
