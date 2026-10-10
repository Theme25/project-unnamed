package
{
   import Levels.Level;
   import flash.display.DisplayObject;
   import flash.display.DisplayObjectContainer;
   import flash.display.MovieClip;
   import flash.display.Sprite;
   import flash.geom.Matrix;
   import flash.geom.Point;
   import flash.geom.Rectangle;
   
   // Practice Hack Shipik with MATHSPIKES 2 (testLevelPoint). Everything else is the original class.
   // MATHSPIKES 2 = MATHSPIKES 1 done in Level coordinates with plain Numbers: no localToGlobal/globalToLocal,
   // so the window size, stage.align, zoom and camera cannot change the result. rbsim mirrors this code
   // operation for operation (src/redball.cpp BallHitsSpikeMath); keep the two in sync.
   [Embed(source="/_assets/assets.swf", symbol="symbol57")]
   public dynamic class Shipik extends MovieClip
   {
      
      private static var h:Number = 9.65;
      
      private static var w:Number = 6;
      
      public var mask:Sprite;
      
      public var cover:Sprite;
      
      public var p:DisplayObjectContainer;
      
      private var m2ready:Boolean = false;
      
      private var m2tx:Number = 0;
      
      private var m2ty:Number = 0;
      
      private var m2ia:Number = 1;
      
      private var m2ib:Number = 0;
      
      private var m2ic:Number = 0;
      
      private var m2id:Number = 1;
      
      private var m2l:Number = 0;
      
      private var m2t:Number = 0;
      
      private var m2w:Number = 0;
      
      private var m2h:Number = 0;
      
      public function Shipik()
      {
         super();
         this.mask = new Sprite();
         this.init();
      }
      
      public function init() : void
      {
         this.cover = new Sprite();
         var coords:Vector.<Number> = new Vector.<Number>(6);
         var p:DisplayObjectContainer = this;
         while(!(p is Level))
         {
            p = p.parent;
            if(p == null)
            {
               return;
            }
         }
         this.p = p;
         var temp:Point = p.globalToLocal(localToGlobal(new Point(0,0)));
         coords[0] = temp.x;
         coords[1] = temp.y;
         temp = p.globalToLocal(localToGlobal(new Point(w / 2,-h)));
         coords[2] = temp.x;
         coords[3] = temp.y;
         temp = p.globalToLocal(localToGlobal(new Point(w,0)));
         coords[4] = temp.x;
         coords[5] = temp.y;
         this.cover.graphics.beginFill(16744448,0.8);
         this.cover.graphics.drawTriangles(coords);
         this.cover.graphics.endFill();
         var bounds:Rectangle = this.getBounds(p);
         this.mask.graphics.beginFill(0,0.5);
         this.mask.graphics.drawRect(bounds.x,bounds.y,bounds.width,bounds.height);
         this.mask.graphics.endFill();
         p.addChild(this.cover);
         p.addChild(this.mask);
         this.cover.mask = this.mask;
      }
      
      public function updateCover(dx:Number, dy:Number) : void
      {
         this.cover.x = dx;
         this.cover.y = dy;
         this.cover.visible = CONFIG.GRAPHICS;
      }
      
      public function testPoint(point:Point) : Boolean
      {
         point = this.p.globalToLocal(point);
         if(!this.getBounds(this.p).containsPoint(point))
         {
            return false;
         }
         var pt:Point = globalToLocal(this.p.localToGlobal(point.subtract(new Point(this.cover.x,this.cover.y))));
         return this.PointInTriangle(pt);
      }
      
      // Level -> Shipik matrix from each clip's own transform.matrix (never concatenatedMatrix, which includes
      // the window), its inverse, and the exact Level-space box of the shape (0,-h)-(w,0). Spikes never move,
      // so this runs once.
      private function prepareMath() : void
      {
         var m:Matrix = this.transform.matrix;
         var a:Number = m.a;
         var b:Number = m.b;
         var c:Number = m.c;
         var d:Number = m.d;
         var tx:Number = Math.round(m.tx * 20);
         var ty:Number = Math.round(m.ty * 20);
         var o:DisplayObject = this.parent;
         var ma:Number = 0;
         var mb:Number = 0;
         var mc:Number = 0;
         var md:Number = 0;
         var mtx:Number = 0;
         var mty:Number = 0;
         var na:Number = 0;
         var nb:Number = 0;
         var nc:Number = 0;
         var nd:Number = 0;
         var ntx:Number = 0;
         var nty:Number = 0;
         while(o != null && o != this.p)
         {
            m = o.transform.matrix;
            ma = m.a;
            mb = m.b;
            mc = m.c;
            md = m.d;
            mtx = Math.round(m.tx * 20);
            mty = Math.round(m.ty * 20);
            na = ma * a + mc * b;
            nb = mb * a + md * b;
            nc = ma * c + mc * d;
            nd = mb * c + md * d;
            ntx = ma * tx + mc * ty + mtx;
            nty = mb * tx + md * ty + mty;
            a = na;
            b = nb;
            c = nc;
            d = nd;
            tx = ntx;
            ty = nty;
            o = o.parent;
         }
         var det:Number = a * d - b * c;
         this.m2ia = d / det;
         this.m2ib = -b / det;
         this.m2ic = -c / det;
         this.m2id = a / det;
         this.m2tx = tx;
         this.m2ty = ty;
         var kx:Array = [0,w,0,w];
         var ky:Array = [-h,-h,0,0];
         var x0:Number = 0;
         var y0:Number = 0;
         var x1:Number = 0;
         var y1:Number = 0;
         var X:Number = 0;
         var Y:Number = 0;
         var k:int = 0;
         while(k < 4)
         {
            X = (a * kx[k] + c * ky[k]) * 20 + tx;
            Y = (b * kx[k] + d * ky[k]) * 20 + ty;
            if(k == 0 || X < x0)
            {
               x0 = X;
            }
            if(k == 0 || X > x1)
            {
               x1 = X;
            }
            if(k == 0 || Y < y0)
            {
               y0 = Y;
            }
            if(k == 0 || Y > y1)
            {
               y1 = Y;
            }
            k++;
         }
         this.m2l = x0 / 20;
         this.m2t = y0 / 20;
         this.m2w = (x1 - x0) / 20;
         this.m2h = (y1 - y0) / 20;
         this.m2ready = true;
      }
      
      // MATHSPIKES 2: (px, py) is a control point in Level coordinates (whole twips, from PlayerBox).
      public function testLevelPoint(px:Number, py:Number) : Boolean
      {
         if(!this.m2ready)
         {
            this.prepareMath();
         }
         if(!(px >= this.m2l && px < this.m2l + this.m2w && py >= this.m2t && py < this.m2t + this.m2h))
         {
            return false;
         }
         var ux0:Number = int((px - this.cover.x) * 20);
         var uy0:Number = int((py - this.cover.y) * 20);
         var ddx:Number = ux0 - this.m2tx;
         var ddy:Number = uy0 - this.m2ty;
         return this.PointInTriangle(new Point((this.m2ia * ddx + this.m2ic * ddy) / 20,(this.m2ib * ddx + this.m2id * ddy) / 20));
      }
      
      public function sign(p1:Point, p2:Point, p3:Point) : Boolean
      {
         return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y) > 0;
      }
      
      public function PointInTriangle(pt:Point) : Boolean
      {
         var v1:Point = new Point(0,0);
         var v2:Point = new Point(w / 2,-h);
         var v3:Point = new Point(w,0);
         var d1:Boolean = this.sign(pt,v1,v2);
         var d2:Boolean = this.sign(pt,v2,v3);
         var d3:Boolean = this.sign(pt,v3,v1);
         return d1 == d2 && d2 == d3;
      }
   }
}
