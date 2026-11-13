import unittest

import model
import emit_cpp


class Sources(unittest.TestCase):
    def source(self, **extra):
        return dict(id=1, kind='source', name='left', type='f32', value_id=256, age_id=257, **extra)

    def load(self, items, name='fusion.fixture'):
        return model.load_items('fusion.fixture.yaml', name, items)

    def test_expansion_and_metadata(self):
        items = self.load([self.source(unit='rad')])
        param, value, age = items
        self.assertEqual((param['kind'], param['type'], param['flags'], param['default_bits']),
                         ('param', 'u32', ['reboot_required', 'persist', 'source'], 0))
        self.assertEqual((value['id'], value['name'], value['type'], value['unit']), (256, 'left.value', 'f32', 'rad'))
        self.assertEqual((age['id'], age['name'], age['type'], age['unit']), (257, 'left.age_us', 'u32', 'us'))
        self.assertEqual(self.load([self.source(persist=False)])[0]['flags'], ['reboot_required', 'source'])
        text = emit_cpp.gen_type(dict(type='fusion.fixture', ns='fusion_fixture', type_id=120,
                                     description='', loops=['realtime'], items=items))
        self.assertIn('ItemFlag::Source', text)
        self.assertIn('kLeftAgeUs', text)

    def test_all_scalar_types_and_enum(self):
        for kind in model.SCALAR_TYPES:
            source = self.source(); source['type'] = kind
            self.assertEqual(self.load([source])[1]['type'], kind)
        source = self.source(); source.update(type='enum', values={'idle': 0, 'run': 1})
        self.assertEqual(len(self.load([source])[1]['values']), 2)

    def test_generated_collisions_and_invalid_declarations(self):
        for extra in [dict(id=256, kind='signal', name='other', type='u32'),
                      dict(id=257, name='old', retired=True),
                      dict(id=500, kind='signal', name='left.value', type='f32'),
                      dict(id=500, kind='signal', name='left_age_us', type='u32')]:
            with self.subTest(extra=extra), self.assertRaises(model.SpecError):
                self.load([self.source(), extra])
        for key, value in [('value_id', 1), ('age_id', 256), ('value_id', -1),
                           ('age_id', 65536), ('type', 'none'), ('name', 'bad name'),
                           ('persist', 3), ('ref', 'feedback'), ('default', 1)]:
            source = self.source(); source[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(model.SpecError):
                self.load([source])
        for name in ['system', 'loop', 'actuator.fixture', 'controller.fixture']:
            with self.subTest(name=name), self.assertRaises(model.SpecError):
                self.load([self.source()], name)


if __name__ == '__main__':
    unittest.main()
