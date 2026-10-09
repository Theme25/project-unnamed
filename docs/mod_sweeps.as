// docs/mod_sweeps.as -- calibration sweeps to add to the logging mod (Game.as), see docs/STATS_LOGGING.md 3.11.
//
// 1. Paste the two functions below into Game.as next to dbgLevelCalib().
// 2. In dbgLevelCalib(), right before the final "Level.lastCheckNum = 0; this.SetLevel(id);", add:
//        this.dbgSpikeSweep(L, out);
//        if (id == 11) this.dbgFlagSweep(L, out);
// 3. Press the level calibration key (]) on Levels 9, 10, 11, 15 and 17 and send the rb1_calib_L<id>.tsv files.
//    The dump is bigger than before (Level 15 a few MB) and may take a minute in the Flash Player.
//
// Both sweeps move only the ball's sprite (the level is paused right after SetLevel, nothing is stepped), with
// the camera cover at (0,0) (dp = 0), exactly as Level.Update would test it.

      // E11: every turned or scaled spike (one per distinct matrix): the ball is placed on a grid around the spike
      // and HitTestObjectControlPoints is logged. rbsim calib replays each row through its spike model.
      private function dbgSpikeSweep(L:*, out:Array) : void
      {
         var pb:* = L.getplayerBox();
         var spikes:Array = [];
         var seen:Object = {};
         var i:int = 0;
         var n:int = 0;
         var o:* = null;
         var s:* = null;
         var m:* = null;
         var key:String = null;
         var idx:int = 0;
         var r:* = null;
         var rot:int = 0;
         var x:Number = NaN;
         var y:Number = NaN;
         var rots:Array = [0,37.3];
         var g:* = null;
         out.push("#E11s\tidx\tgx\tgy\ta\tb\tc\td\t(spike origin in L and its matrix relative to L)");
         out.push("#E11\tidx\trot\tx\ty\thit");
         i = 0;
         while(i < L.numChildren)
         {
            o = L.getChildAt(i);
            if(o is Shipik)
            {
               spikes.push(o);
            }
            else if(o is Ships10)
            {
               n = 0;
               while(n < o.numChildren)
               {
                  if(o.getChildAt(n) is Shipik)
                  {
                     spikes.push(o.getChildAt(n));
                  }
                  n++;
               }
            }
            i++;
         }
         idx = 0;
         for each(s in spikes)
         {
            m = s.transform.matrix.clone();
            if(s.parent != L)
            {
               m.concat(s.parent.transform.matrix);
            }
            if(m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1)
            {
               continue;
            }
            key = [hex(m.a),hex(m.b),hex(m.c),hex(m.d)].join(",");
            if(seen[key])
            {
               continue;
            }
            seen[key] = true;
            g = L.globalToLocal(s.localToGlobal(new Point(0,0)));
            out.push(["E11s",idx,hex(g.x),hex(g.y),hex(m.a),hex(m.b),hex(m.c),hex(m.d)].join("\t"));
            if(s.parent is Ships10)
            {
               s.parent.update(0,0);
            }
            else
            {
               s.updateCover(0,0);
            }
            r = s.getBounds(L);
            rot = 0;
            while(rot < rots.length)
            {
               x = r.x - 11.3;
               while(x <= r.x + r.width + 11.3)
               {
                  y = r.y - 11.3;
                  while(y <= r.y + r.height + 11.3)
                  {
                     pb.x = x;
                     pb.y = y;
                     pb.rotation = rots[rot];
                     out.push(["E11",idx,hex(pb.rotation),hex(pb.x),hex(pb.y),int(pb.HitTestObjectControlPoints(s))].join("\t"));
                     y += 0.41;
                  }
                  x += 0.41;
               }
               rot++;
            }
            idx++;
         }
      }

      // E12 (Level 11 only): the flag turned to several angles; the ball is placed on a grid around it and
      // hitTestObject(levelAim) is logged, plus levelAim.getBounds(L) per angle (E12b).
      private function dbgFlagSweep(L:*, out:Array) : void
      {
         var pb:* = L.getplayerBox();
         var aim:* = L.getChildByName("levelAim");
         var angles:Array = [-4.3213,17.5,39.0404,63.7,91.2,122.6,-151.3,179.4];
         var k:int = 0;
         var r:* = null;
         var x:Number = NaN;
         var y:Number = NaN;
         out.push("#E12b\trot\tax\tay\tx\ty\tw\th\t(levelAim.getBounds(L) after setting levelAim.rotation)");
         out.push("#E12\trot\tax\tay\tbx\tby\thit");
         pb.rotation = 0;
         k = 0;
         while(k < angles.length)
         {
            aim.rotation = angles[k];
            r = aim.getBounds(L);
            out.push(["E12b",hex(aim.rotation),hex(aim.x),hex(aim.y)].concat(this.dbgRect(r)).join("\t"));
            x = r.x - 12.3;
            while(x <= r.x + r.width + 12.3)
            {
               y = r.y - 12.3;
               while(y <= r.y + r.height + 12.3)
               {
                  pb.x = x;
                  pb.y = y;
                  out.push(["E12",hex(angles[k]),hex(aim.x),hex(aim.y),hex(pb.x),hex(pb.y),int(pb.hitTestObject(aim))].join("\t"));
                  y += 0.43;
               }
               x += 0.43;
            }
            k++;
         }
         aim.rotation = 0;
      }
