// SPDX-License-Identifier: GPL-3.0-only
using LibAottg.Assets;
using System.Text;

var package=CharacterPackage.Load(args[0]);
var character=package.CreateInstance();
using var writer=new BinaryWriter(File.Create(args[1]));
writer.Write((uint)(package.Clips.Count*3));
foreach (var clip in package.Clips)
foreach (float fraction in new[]{0f,0.37f,1f}) {
    float time=clip.Duration*fraction;
    var bytes=Encoding.UTF8.GetBytes(clip.Name); writer.Write((uint)bytes.Length); writer.Write(bytes); writer.Write(time);
    character.Sample(clip.Name,time);
    foreach (var primitive in character.Primitives)
    for (int i=0;i<primitive.Positions.Length;i++) {
        var p=primitive.Positions[i]; var n=primitive.Normals[i];
        writer.Write(p.X); writer.Write(p.Y); writer.Write(p.Z); writer.Write(n.X); writer.Write(n.Y); writer.Write(n.Z);
    }
}
Console.WriteLine($"Exported {package.Clips.Count*3} C# reference poses.");
