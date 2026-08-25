"""Multidim VRT fixtures for the mdim convert cases."""

M2 = """<VRTDataset>
  <Group name="/">
    <Attribute name="title" dataType="String">
      <Value>My Title</Value>
    </Attribute>
    <Attribute name="count" dataType="Int32">
      <Value>42</Value>
    </Attribute>
    <Dimension name="Y" size="3" type="HORIZONTAL_Y" direction="NORTH" indexingVariable="Y"/>
    <Dimension name="X" size="4" type="HORIZONTAL_X" direction="EAST" indexingVariable="X"/>
    <Array name="Y">
      <DataType>Float64</DataType>
      <DimensionRef ref="Y"/>
      <RegularlySpacedValues start="30" increment="-1"/>
    </Array>
    <Array name="X">
      <DataType>Float64</DataType>
      <DimensionRef ref="X"/>
      <RegularlySpacedValues start="10" increment="1"/>
    </Array>
    <Array name="temp">
      <DataType>Float32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <SRS dataAxisToSRSAxisMapping="2,1">EPSG:4326</SRS>
      <Unit>K</Unit>
      <NoDataValue>-999</NoDataValue>
      <Offset>1.5</Offset>
      <Scale>0.5</Scale>
      <InlineValues>1 2 3 4 5 6 7 8 9 10 11 -999</InlineValues>
      <Attribute name="long_name" dataType="String">
        <Value>temperature</Value>
      </Attribute>
    </Array>
    <Group name="sub">
      <Dimension name="Z" size="2"/>
      <Array name="press">
        <DataType>Int16</DataType>
        <DimensionRef ref="Z"/>
        <InlineValues>5 6</InlineValues>
      </Array>
    </Group>
  </Group>
</VRTDataset>
"""

TWO = """<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="2"/>
    <Array name="a">
      <DataType>Float64</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>1 2 3 4</InlineValues>
    </Array>
    <Array name="b">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>5 6 7 8</InlineValues>
    </Array>
  </Group>
</VRTDataset>
"""

D3 = """<VRTDataset>
  <Group name="/">
    <Dimension name="T" size="2"/>
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="2"/>
    <Array name="cube">
      <DataType>Int16</DataType>
      <DimensionRef ref="T"/>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>1 2 3 4 5 6 7 8</InlineValues>
    </Array>
  </Group>
</VRTDataset>
"""

ONE1D = """<VRTDataset>
  <Group name="/">
    <Dimension name="N" size="3"/>
    <Array name="v">
      <DataType>Float64</DataType>
      <DimensionRef ref="N"/>
      <InlineValues>1 2 3</InlineValues>
    </Array>
  </Group>
</VRTDataset>
"""


TA = """<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="3" type="HORIZONTAL_Y" direction="NORTH" indexingVariable="Y"/>
    <Dimension name="X" size="4" type="HORIZONTAL_X" direction="EAST" indexingVariable="X"/>
    <Array name="Y">
      <DataType>Float64</DataType>
      <DimensionRef ref="Y"/>
      <RegularlySpacedValues start="30" increment="-1"/>
    </Array>
    <Array name="X">
      <DataType>Float64</DataType>
      <DimensionRef ref="X"/>
      <RegularlySpacedValues start="10" increment="1"/>
    </Array>
    <Array name="temp">
      <DataType>Float32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11</InlineValues>
    </Array>
  </Group>
</VRTDataset>
"""

XREG = '<RegularlySpacedValues start="10" increment="1"/>'
YREG = '<RegularlySpacedValues start="30" increment="-1"/>'


def write_mosaic():
    open("ta.vrt", "w").write(TA)
    open("tb.vrt", "w").write(TA.replace(XREG,
        '<RegularlySpacedValues start="14" increment="1"/>'))
    open("tc.vrt", "w").write(TA.replace(XREG,
        '<RegularlySpacedValues start="16" increment="1"/>'))
    open("td.vrt", "w").write(TA.replace(XREG,
        '<RegularlySpacedValues start="12" increment="1"/>'))
    open("te.vrt", "w").write(TA.replace(YREG,
        '<RegularlySpacedValues start="27" increment="-1"/>'))
    open("tj.vrt", "w").write(TA.replace(XREG,
        '<InlineValues>10 11 13 17</InlineValues>'))
    open("tn.vrt", "w").write(TA.replace(XREG,
        '<InlineValues>10 11 13 18</InlineValues>'))
    open("tk.vrt", "w").write(TA.replace(XREG,
        '<RegularlySpacedValues start="10" increment="2"/>'))
    open("tl.vrt", "w").write(
        TA.replace('<DataType>Float32</DataType>',
                   '<DataType>Float64</DataType>', 2)
          .replace('<DataType>Float64</DataType>',
                   '<DataType>Float32</DataType>', 2))
    open("tw.vrt", "w").write(
        TA.replace('name="X" size="4"', 'name="X" size="1"')
          .replace('<InlineValues>0 1 2 3 4 5 6 7 8 9 10 11</InlineValues>',
                   '<InlineValues>0 1 2</InlineValues>'))
    open("y28.vrt", "w").write(TA.replace(YREG,
        '<RegularlySpacedValues start="28" increment="-1"/>'))
    open("y33.vrt", "w").write(TA.replace(YREG,
        '<RegularlySpacedValues start="33" increment="-1"/>'))
    tm = TA.replace('<Array name="temp">', '<Array name="hum">', 1)
    open("tm.vrt", "w").write(tm)
    open("two3.vrt", "w").write(
        TWO.replace('<Dimension name="Y" size="2"/>',
                    '<Dimension name="Y" size="3"/>')
           .replace('<InlineValues>1 2 3 4</InlineValues>',
                    '<InlineValues>1 2 3 4 5 6</InlineValues>')
           .replace('<InlineValues>5 6 7 8</InlineValues>',
                    '<InlineValues>5 6 7 8 9 10</InlineValues>'))
    off = """<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="3"/>
    <Dimension name="X" size="4"/>
    <Array name="temp">
      <DataType>Float32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename relativetoVRT="1">ta.vrt</SourceFilename>
        <SourceArray>/temp</SourceArray>
        <SourceSlab offset="0,0" count="3,4" step="1,1" />
        <DestSlab offset="3,0" />
      </Source>
    </Array>
  </Group>
</VRTDataset>
"""
    open("off2.vrt", "w").write(off)


def write_all():
    open("m2.vrt", "w").write(M2)
    open("two.vrt", "w").write(TWO)
    open("d3.vrt", "w").write(D3)
    open("one1d.vrt", "w").write(ONE1D)
    import sys
    sys.path.insert(0, "/workspace/tests")
    from mk_tail_fixtures import write_tif
    write_tif("ct.tif", "Byte", 4, 3, [[str(i * 20) for i in range(12)]],
              gt=(10, 1, 0, 30, 0, -1), epsg=4326)
