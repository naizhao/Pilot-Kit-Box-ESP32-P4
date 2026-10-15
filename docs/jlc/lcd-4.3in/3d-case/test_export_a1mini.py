"""检查导出器确实拒绝破面、反面和超出打印体积的网格。"""
import unittest

from export_a1mini import validate_mesh


def tetrahedron(offset=0, scale=1):
    a, b, c, d = [(x * scale + offset, y * scale, z * scale)
                  for x, y, z in [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]]
    return [(a, c, b), (a, b, d), (a, d, c), (b, c, d)]


class MeshValidationTest(unittest.TestCase):
    def test_closed_solid_reports_size_and_volume(self):
        result = validate_mesh(tetrahedron())
        self.assertEqual(result['size_mm'], [1, 1, 1])
        self.assertEqual(result['components'], 1)
        self.assertAlmostEqual(result['volume_mm3'], 1 / 6)

    def test_missing_face_is_rejected(self):
        with self.assertRaisesRegex(ValueError, '闭合'):
            validate_mesh(tetrahedron()[:-1])

    def test_reversed_face_is_rejected(self):
        triangles = tetrahedron()
        triangles[0] = tuple(reversed(triangles[0]))
        with self.assertRaisesRegex(ValueError, '绕序'):
            validate_mesh(triangles)

    def test_disconnected_case_is_rejected(self):
        with self.assertRaisesRegex(ValueError, '连通'):
            validate_mesh(tetrahedron() + tetrahedron(offset=3))

    def test_plunger_array_can_have_separate_solids(self):
        result = validate_mesh(tetrahedron() + tetrahedron(offset=3), expected_components=2)
        self.assertEqual(result['components'], 2)

    def test_a1mini_oversize_is_rejected(self):
        with self.assertRaisesRegex(ValueError, '180'):
            validate_mesh(tetrahedron(scale=181))


if __name__ == '__main__':
    unittest.main()
